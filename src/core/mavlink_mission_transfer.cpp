#include <algorithm>
#include "mavlink_mission_transfer.h"
#include "log.h"

namespace mavsdk {

MAVLinkMissionTransfer::MAVLinkMissionTransfer(
    Sender& sender, MAVLinkMessageHandler& message_handler, TimeoutHandler& timeout_handler) :
    _sender(sender),
    _message_handler(message_handler),
    _timeout_handler(timeout_handler)
{}

MAVLinkMissionTransfer::~MAVLinkMissionTransfer() {}

std::weak_ptr<MAVLinkMissionTransfer::WorkItem> MAVLinkMissionTransfer::upload_items_async(
    uint8_t type, const std::vector<ItemInt>& items, ResultCallback callback)
{
    auto ptr = std::make_shared<UploadWorkItem>(
        _sender, _message_handler, _timeout_handler, type, items, callback);

    _work_queue.push_back(ptr);

    return std::weak_ptr<WorkItem>(ptr);
}

std::weak_ptr<MAVLinkMissionTransfer::WorkItem>
MAVLinkMissionTransfer::download_items_async(uint8_t type, ResultAndItemsCallback callback)
{
    auto ptr = std::make_shared<DownloadWorkItem>(
        _sender, _message_handler, _timeout_handler, type, callback);

    _work_queue.push_back(ptr);

    return std::weak_ptr<WorkItem>(ptr);
}

void MAVLinkMissionTransfer::clear_items_async(uint8_t type, ResultCallback callback)
{
    auto ptr = std::make_shared<ClearWorkItem>(
        _sender, _message_handler, _timeout_handler, type, callback);

    _work_queue.push_back(ptr);
}

void MAVLinkMissionTransfer::set_current_item_async(int current, ResultCallback callback)
{
    auto ptr = std::make_shared<SetCurrentWorkItem>(
        _sender, _message_handler, _timeout_handler, current, callback);

    _work_queue.push_back(ptr);
}

void MAVLinkMissionTransfer::do_work()
{
    LockedQueue<WorkItem>::Guard work_queue_guard(_work_queue);
    auto work = work_queue_guard.get_front();

    if (!work) {
        return;
    }

    if (!work->has_started()) {
        work->start();
    }
    if (work->is_done()) {
        work_queue_guard.pop_front();
    }
}

bool MAVLinkMissionTransfer::is_idle()
{
    LockedQueue<WorkItem>::Guard work_queue_guard(_work_queue);
    return (work_queue_guard.get_front() == nullptr);
}

MAVLinkMissionTransfer::WorkItem::WorkItem(
    Sender& sender,
    MAVLinkMessageHandler& message_handler,
    TimeoutHandler& timeout_handler,
    uint8_t type) :
    _sender(sender),
    _message_handler(message_handler),
    _timeout_handler(timeout_handler),
    _type(type)
{}

MAVLinkMissionTransfer::WorkItem::~WorkItem() {}

bool MAVLinkMissionTransfer::WorkItem::has_started()
{
    std::lock_guard<std::mutex> lock(_mutex);
    return _started;
}

bool MAVLinkMissionTransfer::WorkItem::is_done()
{
    std::lock_guard<std::mutex> lock(_mutex);
    return _done;
}

MAVLinkMissionTransfer::UploadWorkItem::UploadWorkItem(
    Sender& sender,
    MAVLinkMessageHandler& message_handler,
    TimeoutHandler& timeout_handler,
    uint8_t type,
    const std::vector<ItemInt>& items,
    ResultCallback callback) :
    WorkItem(sender, message_handler, timeout_handler, type),
    _items(items),
    _callback(callback)
{
    std::lock_guard<std::mutex> lock(_mutex);

    _message_handler.register_one(
        MAVLINK_MSG_ID_MISSION_REQUEST,
        [this](const mavlink_message_t& message) { process_mission_request(message); },
        this);

    _message_handler.register_one(
        MAVLINK_MSG_ID_MISSION_REQUEST_INT,
        [this](const mavlink_message_t& message) { process_mission_request_int(message); },
        this);

    _message_handler.register_one(
        MAVLINK_MSG_ID_MISSION_ACK,
        [this](const mavlink_message_t& message) { process_mission_ack(message); },
        this);
}

MAVLinkMissionTransfer::UploadWorkItem::~UploadWorkItem()
{
    std::lock_guard<std::mutex> lock(_mutex);
    _message_handler.unregister_all(this);
    _timeout_handler.remove(_cookie);
}

void MAVLinkMissionTransfer::UploadWorkItem::start()
{
    std::lock_guard<std::mutex> lock(_mutex);

    _started = true;
    if (_items.size() == 0) {
        callback_and_reset(Result::NoMissionAvailable);
        return;
    }

    int count = 0;
    for (const auto& item : _items) {
        if (count++ != item.seq) {
            callback_and_reset(Result::InvalidSequence);
            return;
        }
    }

    int num_currents = 0;
    std::for_each(_items.cbegin(), _items.cend(), [&num_currents](const ItemInt& item) {
        num_currents += item.current;
    });
    if (num_currents != 1) {
        callback_and_reset(Result::CurrentInvalid);
        return;
    }

    if (std::any_of(_items.cbegin(), _items.cend(), [this](const ItemInt& item) {
            return item.mission_type != _type;
        })) {
        callback_and_reset(Result::MissionTypeNotConsistent);
        return;
    }

    _retries_done = 0;
    _step = Step::SendCount;
    _timeout_handler.add([this]() { process_timeout(); }, timeout_s, &_cookie);

    _next_sequence = 0;

    send_count();
}

void MAVLinkMissionTransfer::UploadWorkItem::cancel()
{
    std::lock_guard<std::mutex> lock(_mutex);

    _timeout_handler.remove(_cookie);
    send_cancel_and_finish();
}

void MAVLinkMissionTransfer::UploadWorkItem::send_count()
{
    mavlink_message_t message;
    mavlink_msg_mission_count_pack(
        _sender.own_address.system_id,
        _sender.own_address.component_id,
        &message,
        _sender.target_address.system_id,
        _sender.target_address.component_id,
        _items.size(),
        _type);

    if (!_sender.send_message(message)) {
        _timeout_handler.remove(_cookie);
        callback_and_reset(Result::ConnectionError);
        return;
    }

    ++_retries_done;
}

void MAVLinkMissionTransfer::UploadWorkItem::send_cancel_and_finish()
{
    mavlink_message_t message;
    mavlink_msg_mission_ack_pack(
        _sender.own_address.system_id,
        _sender.own_address.component_id,
        &message,
        _sender.target_address.system_id,
        _sender.target_address.component_id,
        MAV_MISSION_OPERATION_CANCELLED,
        _type);

    if (!_sender.send_message(message)) {
        callback_and_reset(Result::ConnectionError);
        return;
    }

    // We do not wait on anything coming back after this.
    callback_and_reset(Result::Cancelled);
}

void MAVLinkMissionTransfer::UploadWorkItem::process_mission_request(
    const mavlink_message_t& unused)
{
    std::lock_guard<std::mutex> lock(_mutex);

    // We only support int, so we nack this and thus tell the autopilot to use int.
    UNUSED(unused);

    mavlink_message_t message;
    mavlink_msg_mission_ack_pack(
        _sender.own_address.system_id,
        _sender.own_address.component_id,
        &message,
        _sender.target_address.system_id,
        _sender.target_address.component_id,
        MAV_MISSION_UNSUPPORTED,
        _type);

    if (!_sender.send_message(message)) {
        _timeout_handler.remove(_cookie);
        callback_and_reset(Result::ConnectionError);
        return;
    }

    _timeout_handler.refresh(_cookie);
}

void MAVLinkMissionTransfer::UploadWorkItem::process_mission_request_int(
    const mavlink_message_t& message)
{
    std::lock_guard<std::mutex> lock(_mutex);

    mavlink_mission_request_int_t request_int;
    mavlink_msg_mission_request_int_decode(&message, &request_int);

    _step = Step::SendItems;

    if (_next_sequence < request_int.seq) {
        // We should not go back to a previous one.
        // TODO: figure out if we should error here.
        LogWarn() << "mission_request_int: sequence incorrect";
        return;

    } else if (_next_sequence > request_int.seq) {
        // We have already sent that one before.
        if (_retries_done >= retries) {
            _timeout_handler.remove(_cookie);
            callback_and_reset(Result::Timeout);
            return;
        }

    } else {
        // Correct one, sending it the first time.
        _retries_done = 0;
    }

    _timeout_handler.refresh(_cookie);

    _next_sequence = request_int.seq;
    send_mission_item();
}

void MAVLinkMissionTransfer::UploadWorkItem::send_mission_item()
{
    if (_next_sequence >= _items.size()) {
        LogErr() << "send_mission_item: sequence out of bounds";
        return;
    }

    mavlink_message_t message;
    mavlink_msg_mission_item_int_pack(
        _sender.own_address.system_id,
        _sender.own_address.component_id,
        &message,
        _sender.target_address.system_id,
        _sender.target_address.component_id,
        _next_sequence,
        _items[_next_sequence].frame,
        _items[_next_sequence].command,
        _items[_next_sequence].current,
        _items[_next_sequence].autocontinue,
        _items[_next_sequence].param1,
        _items[_next_sequence].param2,
        _items[_next_sequence].param3,
        _items[_next_sequence].param4,
        _items[_next_sequence].x,
        _items[_next_sequence].y,
        _items[_next_sequence].z,
        _type);

    ++_next_sequence;

    if (!_sender.send_message(message)) {
        _timeout_handler.remove(_cookie);
        callback_and_reset(Result::ConnectionError);
        return;
    }

    ++_retries_done;
}

void MAVLinkMissionTransfer::UploadWorkItem::process_mission_ack(const mavlink_message_t& message)
{
    std::lock_guard<std::mutex> lock(_mutex);

    mavlink_mission_ack_t mission_ack;
    mavlink_msg_mission_ack_decode(&message, &mission_ack);

    _timeout_handler.remove(_cookie);

    switch (mission_ack.type) {
        case MAV_MISSION_ERROR:
            callback_and_reset(Result::ProtocolError);
            return;
        case MAV_MISSION_UNSUPPORTED_FRAME:
            callback_and_reset(Result::UnsupportedFrame);
            return;
        case MAV_MISSION_UNSUPPORTED:
            callback_and_reset(Result::Unsupported);
            return;
        case MAV_MISSION_NO_SPACE:
            callback_and_reset(Result::TooManyMissionItems);
            return;
        case MAV_MISSION_INVALID:
            // FALLTHROUGH
        case MAV_MISSION_INVALID_PARAM1:
            // FALLTHROUGH
        case MAV_MISSION_INVALID_PARAM2:
            // FALLTHROUGH
        case MAV_MISSION_INVALID_PARAM3:
            // FALLTHROUGH
        case MAV_MISSION_INVALID_PARAM4:
            // FALLTHROUGH
        case MAV_MISSION_INVALID_PARAM5_X:
            // FALLTHROUGH
        case MAV_MISSION_INVALID_PARAM6_Y:
            // FALLTHROUGH
        case MAV_MISSION_INVALID_PARAM7:
            callback_and_reset(Result::InvalidParam);
            return;
        case MAV_MISSION_INVALID_SEQUENCE:
            callback_and_reset(Result::InvalidSequence);
            return;
        case MAV_MISSION_DENIED:
            callback_and_reset(Result::Denied);
            return;
        case MAV_MISSION_OPERATION_CANCELLED:
            callback_and_reset(Result::Cancelled);
            return;
    }

    if (_next_sequence == _items.size()) {
        callback_and_reset(Result::Success);
    } else {
        callback_and_reset(Result::ProtocolError);
    }
}

void MAVLinkMissionTransfer::UploadWorkItem::process_timeout()
{
    std::lock_guard<std::mutex> lock(_mutex);

    if (_retries_done >= retries) {
        callback_and_reset(Result::Timeout);
        return;
    }

    switch (_step) {
        case Step::SendCount:
            _timeout_handler.add([this]() { process_timeout(); }, timeout_s, &_cookie);
            send_count();
            break;

        case Step::SendItems:
            callback_and_reset(Result::Timeout);
            break;
    }
}

void MAVLinkMissionTransfer::UploadWorkItem::callback_and_reset(Result result)
{
    if (_callback) {
        _callback(result);
    }
    _callback = nullptr;
    _done = true;
}

MAVLinkMissionTransfer::DownloadWorkItem::DownloadWorkItem(
    Sender& sender,
    MAVLinkMessageHandler& message_handler,
    TimeoutHandler& timeout_handler,
    uint8_t type,
    ResultAndItemsCallback callback) :
    WorkItem(sender, message_handler, timeout_handler, type),
    _callback(callback)
{
    std::lock_guard<std::mutex> lock(_mutex);

    _message_handler.register_one(
        MAVLINK_MSG_ID_MISSION_COUNT,
        [this](const mavlink_message_t& message) { process_mission_count(message); },
        this);

    _message_handler.register_one(
        MAVLINK_MSG_ID_MISSION_ITEM_INT,
        [this](const mavlink_message_t& message) { process_mission_item_int(message); },
        this);
}

MAVLinkMissionTransfer::DownloadWorkItem::~DownloadWorkItem()
{
    std::lock_guard<std::mutex> lock(_mutex);

    _message_handler.unregister_all(this);
    _timeout_handler.remove(_cookie);
}

void MAVLinkMissionTransfer::DownloadWorkItem::start()
{
    std::lock_guard<std::mutex> lock(_mutex);

    _items.clear();
    _started = true;
    _retries_done = 0;
    _timeout_handler.add([this]() { process_timeout(); }, timeout_s, &_cookie);
    request_list();
}

void MAVLinkMissionTransfer::DownloadWorkItem::cancel()
{
    std::lock_guard<std::mutex> lock(_mutex);

    _timeout_handler.remove(_cookie);
    send_cancel_and_finish();
}

void MAVLinkMissionTransfer::DownloadWorkItem::request_list()
{
    mavlink_message_t message;
    mavlink_msg_mission_request_list_pack(
        _sender.own_address.system_id,
        _sender.own_address.component_id,
        &message,
        _sender.target_address.system_id,
        _sender.target_address.component_id,
        _type);

    if (!_sender.send_message(message)) {
        _timeout_handler.remove(_cookie);
        callback_and_reset(Result::ConnectionError);
        return;
    }

    ++_retries_done;
}

void MAVLinkMissionTransfer::DownloadWorkItem::request_item()
{
    mavlink_message_t message;
    mavlink_msg_mission_request_int_pack(
        _sender.own_address.system_id,
        _sender.own_address.component_id,
        &message,
        _sender.target_address.system_id,
        _sender.target_address.component_id,
        _next_sequence,
        _type);

    if (!_sender.send_message(message)) {
        _timeout_handler.remove(_cookie);
        callback_and_reset(Result::ConnectionError);
        return;
    }

    ++_retries_done;
}

void MAVLinkMissionTransfer::DownloadWorkItem::send_ack_and_finish()
{
    mavlink_message_t message;
    mavlink_msg_mission_ack_pack(
        _sender.own_address.system_id,
        _sender.own_address.component_id,
        &message,
        _sender.target_address.system_id,
        _sender.target_address.component_id,
        MAV_MISSION_ACCEPTED,
        _type);

    if (!_sender.send_message(message)) {
        callback_and_reset(Result::ConnectionError);
        return;
    }

    // We do not wait on anything coming back after this.
    callback_and_reset(Result::Success);
}

void MAVLinkMissionTransfer::DownloadWorkItem::send_cancel_and_finish()
{
    mavlink_message_t message;
    mavlink_msg_mission_ack_pack(
        _sender.own_address.system_id,
        _sender.own_address.component_id,
        &message,
        _sender.target_address.system_id,
        _sender.target_address.component_id,
        MAV_MISSION_OPERATION_CANCELLED,
        _type);

    if (!_sender.send_message(message)) {
        callback_and_reset(Result::ConnectionError);
        return;
    }

    // We do not wait on anything coming back after this.
    callback_and_reset(Result::Cancelled);
}

void MAVLinkMissionTransfer::DownloadWorkItem::process_mission_count(
    const mavlink_message_t& message)
{
    std::lock_guard<std::mutex> lock(_mutex);

    mavlink_mission_count_t count;
    mavlink_msg_mission_count_decode(&message, &count);

    if (count.count == 0) {
        send_ack_and_finish();
        _timeout_handler.remove(_cookie);
        return;
    }

    _timeout_handler.refresh(_cookie);
    _next_sequence = 0;
    _step = Step::RequestItem;
    _retries_done = 0;
    _expected_count = count.count;
    request_item();
}

void MAVLinkMissionTransfer::DownloadWorkItem::process_mission_item_int(
    const mavlink_message_t& message)
{
    std::lock_guard<std::mutex> lock(_mutex);

    _timeout_handler.refresh(_cookie);

    mavlink_mission_item_int_t item_int;
    mavlink_msg_mission_item_int_decode(&message, &item_int);

    _items.push_back(ItemInt{
        item_int.seq,
        item_int.frame,
        item_int.command,
        item_int.current,
        item_int.autocontinue,
        item_int.param1,
        item_int.param2,
        item_int.param3,
        item_int.param4,
        item_int.x,
        item_int.y,
        item_int.z,
        item_int.mission_type});

    if (_next_sequence + 1 == _expected_count) {
        _timeout_handler.remove(_cookie);
        send_ack_and_finish();

    } else {
        _next_sequence = item_int.seq + 1;
        _retries_done = 0;
        request_item();
    }
}

void MAVLinkMissionTransfer::DownloadWorkItem::process_timeout()
{
    std::lock_guard<std::mutex> lock(_mutex);

    if (_retries_done >= retries) {
        callback_and_reset(Result::Timeout);
        return;
    }

    switch (_step) {
        case Step::RequestList:
            _timeout_handler.add([this]() { process_timeout(); }, timeout_s, &_cookie);
            request_list();
            break;

        case Step::RequestItem:
            _timeout_handler.add([this]() { process_timeout(); }, timeout_s, &_cookie);
            request_item();
            break;
    }
}

void MAVLinkMissionTransfer::DownloadWorkItem::callback_and_reset(Result result)
{
    if (_callback) {
        _callback(result, _items);
    }
    _callback = nullptr;
    _done = true;
}

MAVLinkMissionTransfer::ClearWorkItem::ClearWorkItem(
    Sender& sender,
    MAVLinkMessageHandler& message_handler,
    TimeoutHandler& timeout_handler,
    uint8_t type,
    ResultCallback callback) :
    WorkItem(sender, message_handler, timeout_handler, type),
    _callback(callback)
{
    std::lock_guard<std::mutex> lock(_mutex);

    _message_handler.register_one(
        MAVLINK_MSG_ID_MISSION_ACK,
        [this](const mavlink_message_t& message) { process_mission_ack(message); },
        this);
}

MAVLinkMissionTransfer::ClearWorkItem::~ClearWorkItem()
{
    std::lock_guard<std::mutex> lock(_mutex);

    _message_handler.unregister_all(this);
    _timeout_handler.remove(_cookie);
}

void MAVLinkMissionTransfer::ClearWorkItem::start()
{
    std::lock_guard<std::mutex> lock(_mutex);

    _started = true;
    _retries_done = 0;
    _timeout_handler.add([this]() { process_timeout(); }, timeout_s, &_cookie);
    send_clear();
}

void MAVLinkMissionTransfer::ClearWorkItem::cancel()
{
    std::lock_guard<std::mutex> lock(_mutex);

    _timeout_handler.remove(_cookie);
    // This is presumably not used or exposed because it is quick.
}

void MAVLinkMissionTransfer::ClearWorkItem::send_clear()
{
    mavlink_message_t message;
    mavlink_msg_mission_clear_all_pack(
        _sender.own_address.system_id,
        _sender.own_address.component_id,
        &message,
        _sender.target_address.system_id,
        _sender.target_address.component_id,
        _type);

    if (!_sender.send_message(message)) {
        _timeout_handler.remove(_cookie);
        callback_and_reset(Result::ConnectionError);
        return;
    }

    ++_retries_done;
}

void MAVLinkMissionTransfer::ClearWorkItem::process_timeout()
{
    std::lock_guard<std::mutex> lock(_mutex);

    if (_retries_done >= retries) {
        callback_and_reset(Result::Timeout);
        return;
    }

    _timeout_handler.add([this]() { process_timeout(); }, timeout_s, &_cookie);
    send_clear();
}

void MAVLinkMissionTransfer::ClearWorkItem::process_mission_ack(const mavlink_message_t& message)
{
    std::lock_guard<std::mutex> lock(_mutex);

    mavlink_mission_ack_t mission_ack;
    mavlink_msg_mission_ack_decode(&message, &mission_ack);

    _timeout_handler.remove(_cookie);

    switch (mission_ack.type) {
        case MAV_MISSION_ACCEPTED:
            callback_and_reset(Result::Success);
            return;
        case MAV_MISSION_ERROR:
            callback_and_reset(Result::ProtocolError);
            return;
        case MAV_MISSION_UNSUPPORTED_FRAME:
            callback_and_reset(Result::UnsupportedFrame);
            return;
        case MAV_MISSION_UNSUPPORTED:
            callback_and_reset(Result::Unsupported);
            return;
        case MAV_MISSION_NO_SPACE:
            callback_and_reset(Result::TooManyMissionItems);
            return;
        case MAV_MISSION_INVALID:
            // FALLTHROUGH
        case MAV_MISSION_INVALID_PARAM1:
            // FALLTHROUGH
        case MAV_MISSION_INVALID_PARAM2:
            // FALLTHROUGH
        case MAV_MISSION_INVALID_PARAM3:
            // FALLTHROUGH
        case MAV_MISSION_INVALID_PARAM4:
            // FALLTHROUGH
        case MAV_MISSION_INVALID_PARAM5_X:
            // FALLTHROUGH
        case MAV_MISSION_INVALID_PARAM6_Y:
            // FALLTHROUGH
        case MAV_MISSION_INVALID_PARAM7:
            callback_and_reset(Result::InvalidParam);
            return;
        case MAV_MISSION_INVALID_SEQUENCE:
            callback_and_reset(Result::InvalidSequence);
            return;
        case MAV_MISSION_DENIED:
            callback_and_reset(Result::Denied);
            return;
        case MAV_MISSION_OPERATION_CANCELLED:
            callback_and_reset(Result::Cancelled);
            return;
    }
}

void MAVLinkMissionTransfer::ClearWorkItem::callback_and_reset(Result result)
{
    if (_callback) {
        _callback(result);
    }
    _callback = nullptr;
    _done = true;
}

MAVLinkMissionTransfer::SetCurrentWorkItem::SetCurrentWorkItem(
    Sender& sender,
    MAVLinkMessageHandler& message_handler,
    TimeoutHandler& timeout_handler,
    int current,
    ResultCallback callback) :
    WorkItem(sender, message_handler, timeout_handler, MAV_MISSION_TYPE_MISSION),
    _current(current),
    _callback(callback)
{
    std::lock_guard<std::mutex> lock(_mutex);

    _message_handler.register_one(
        MAVLINK_MSG_ID_MISSION_CURRENT,
        [this](const mavlink_message_t& message) { process_mission_current(message); },
        this);
}

MAVLinkMissionTransfer::SetCurrentWorkItem::~SetCurrentWorkItem()
{
    std::lock_guard<std::mutex> lock(_mutex);
    _message_handler.unregister_all(this);
    _timeout_handler.remove(_cookie);
}

void MAVLinkMissionTransfer::SetCurrentWorkItem::start()
{
    std::lock_guard<std::mutex> lock(_mutex);

    _started = true;

    // If we coudln't find it, the requested item is out of range and probably an invalid argument.
    if (_current < 0) {
        callback_and_reset(Result::CurrentInvalid);
        return;
    }

    _retries_done = 0;
    _timeout_handler.add([this]() { process_timeout(); }, timeout_s, &_cookie);
    send_current_mission_item();
}

void MAVLinkMissionTransfer::SetCurrentWorkItem::cancel()
{
    std::lock_guard<std::mutex> lock(_mutex);

    _timeout_handler.remove(_cookie);
    // This is presumably not used or exposed because it is quick.
}

void MAVLinkMissionTransfer::SetCurrentWorkItem::send_current_mission_item()
{
    mavlink_message_t message;
    mavlink_msg_mission_set_current_pack(
        _sender.own_address.system_id,
        _sender.own_address.component_id,
        &message,
        _sender.target_address.system_id,
        _sender.target_address.component_id,
        _current);

    if (!_sender.send_message(message)) {
        _timeout_handler.remove(_cookie);
        callback_and_reset(Result::ConnectionError);
        return;
    }

    ++_retries_done;
}

void MAVLinkMissionTransfer::SetCurrentWorkItem::process_mission_current(
    const mavlink_message_t& message)
{
    std::lock_guard<std::mutex> lock(_mutex);

    mavlink_mission_current_t mission_current;
    mavlink_msg_mission_current_decode(&message, &mission_current);

    _timeout_handler.remove(_cookie);

    if (_current == mission_current.seq) {
        _current = mission_current.seq;
        callback_and_reset(Result::Success);
        return;
    } else {
        _timeout_handler.refresh(&_cookie);
        send_current_mission_item();
        return;
    }
}

void MAVLinkMissionTransfer::SetCurrentWorkItem::process_timeout()
{
    std::lock_guard<std::mutex> lock(_mutex);

    if (_retries_done >= retries) {
        callback_and_reset(Result::Timeout);
        return;
    }

    _timeout_handler.add([this]() { process_timeout(); }, timeout_s, &_cookie);
    send_current_mission_item();
}

void MAVLinkMissionTransfer::SetCurrentWorkItem::callback_and_reset(Result result)
{
    if (_callback) {
        _callback(result);
    }
    _callback = nullptr;
    _done = true;
}
} // namespace mavsdk
