#pragma once

#include <stddef.h>
#include <string.h>
#include <atomic>

#include <frg/container_of.hpp>
#include <frigg/array.hpp>
#include <frigg/linked.hpp>
#include <frigg/vector.hpp>
#include <thor-internal/core.hpp>
#include <thor-internal/error.hpp>
#include <thor-internal/kernel_heap.hpp>

namespace thor {

struct StreamPacket {
	friend struct Stream;
	friend struct StreamNode;

	StreamPacket()
	: _incompleteCount{0} { }

	void setup(unsigned int count, Worklet *transmitted) {
		_incompleteCount.store(count, std::memory_order_relaxed);
		_transmitted = transmitted;
	}

private:
	Worklet *_transmitted;

	std::atomic<unsigned int> _incompleteCount;
};

enum {
	kTagNull,
	kTagOffer,
	kTagAccept,
	kTagImbueCredentials,
	kTagExtractCredentials,
	kTagSendFromBuffer,
	kTagRecvInline,
	kTagRecvToBuffer,
	kTagPushDescriptor,
	kTagPullDescriptor
};

inline int getStreamOrientation(int tag) {
	switch(tag) {
	case kTagAccept:
	case kTagExtractCredentials:
	case kTagRecvInline:
	case kTagRecvToBuffer:
	case kTagPullDescriptor:
		return -1;
	case kTagOffer:
	case kTagImbueCredentials:
	case kTagSendFromBuffer:
	case kTagPushDescriptor:
		return 1;
	}
	return 0;
}

struct StreamNode {
	friend struct Stream;

	StreamNode() = default;

	StreamNode(const StreamNode &) = delete;

	StreamNode &operator= (const StreamNode &) = delete;

	int tag() const {
		return _tag;
	}

	void setup(int tag, StreamPacket *packet) {
		_tag = tag;
		_packet = packet;
	}

	frg::default_list_hook<StreamNode> processQueueItem;

	void complete() {
		auto n = _packet->_incompleteCount.fetch_sub(1, std::memory_order_acq_rel);
		assert(n > 0);
		if(n == 1)
			WorkQueue::post(_packet->_transmitted);
	}

	LaneHandle _transmitLane;

private:
	int _tag;
	StreamPacket *_packet;

public:
	// ------------------------------------------------------------------------
	// Transmission inputs.
	// ------------------------------------------------------------------------

	frigg::Array<char, 16> _inCredentials;
	size_t _maxLength;
	frigg::UniqueMemory<KernelAlloc> _inBuffer;
	AnyBufferAccessor _inAccessor;
	AnyDescriptor _inDescriptor;

	// List of StreamNodes that will be submitted to the ancillary lane on offer/accept.
	frg::intrusive_list<
		StreamNode,
		frg::locate_member<
			StreamNode,
			frg::default_list_hook<StreamNode>,
			&StreamNode::processQueueItem
		>
	> ancillaryChain;

	// ------------------------------------------------------------------------
	// Transmission outputs.
	// ------------------------------------------------------------------------

	// TODO: Initialize outputs to zero to avoid leaks to usermode.
public:
	Error error() {
		return _error;
	}

	frigg::Array<char, 16> credentials() {
		return _transmitCredentials;
	}

	size_t actualLength() {
		return _actualLength;
	}

	frigg::UniqueMemory<KernelAlloc> transmitBuffer() {
		return std::move(_transmitBuffer);
	}

	const frigg::Array<char, 16> &transmitCredentials() {
		return _transmitCredentials;
	}

	LaneHandle lane() {
		return std::move(_lane);
	}

	AnyDescriptor descriptor() {
		return std::move(_descriptor);
	}

public:
	Error _error;
	frigg::Array<char, 16> _transmitCredentials;
	size_t _actualLength;
	frigg::UniqueMemory<KernelAlloc> _transmitBuffer;
	LaneHandle _lane;
	AnyDescriptor _descriptor;
};

using StreamList = frg::intrusive_list<
	StreamNode,
	frg::locate_member<
		StreamNode,
		frg::default_list_hook<StreamNode>,
		&StreamNode::processQueueItem
	>
>;

struct Stream {
	struct Submitter {
		void enqueue(const LaneHandle &lane, StreamList &chain);

		void run();

	private:
		StreamList _pending;
	};

	// manage the peer counter of each lane.
	// incrementing a peer counter that is already at zero is undefined.
	// decrementPeers() returns true if the counter reaches zero.
	static void incrementPeers(Stream *stream, int lane);
	static bool decrementPeers(Stream *stream, int lane);

	Stream();
	~Stream();

	// Submits a chain of operations to the stream.
	static void transmit(const LaneHandle &lane, StreamList &chain) {
		Submitter submitter;
		submitter.enqueue(lane, chain);
		submitter.run();
	}

	void shutdownLane(int lane);

private:
	static void _cancelItem(StreamNode *item, Error error);

	std::atomic<int> _peerCount[2];

	frigg::TicketLock _mutex;

	// protected by _mutex.
	frg::intrusive_list<
		StreamNode,
		frg::locate_member<
			StreamNode,
			frg::default_list_hook<StreamNode>,
			&StreamNode::processQueueItem
		>
	> _processQueue[2];

	// Protected by _mutex.
	// Further submissions cannot happen (lane went out-of-scope).
	// Submissions to the paired lane return end-of-lane errors.
	bool _laneBroken[2];
	// Submissions are disallowed and return lane-shutdown errors.
	// Submissions to the paired lane return end-of-lane errors.
	bool _laneShutDown[2];
};

frg::tuple<LaneHandle, LaneHandle> createStream();

//---------------------------------------------------------------------------------------
// In-kernel stream utilities.
// Those are only used internally; not by the hel API.
//---------------------------------------------------------------------------------------

struct OfferSender {
	LaneHandle lane;
};

template<typename R>
struct OfferOperation : private StreamPacket, private StreamNode {
	void start() {
		worklet_.setup([] (Worklet *base) {
			auto self = frg::container_of(base, &OfferOperation::worklet_);
			async::execution::set_value(self->receiver_,
					frg::make_tuple(self->error(), self->lane()));
		});
		StreamPacket::setup(1, &worklet_);
		StreamNode::setup(kTagOffer, this);

		StreamList list;
		list.push_back(this);
		Stream::transmit(s_.lane, list);
	}

	OfferOperation(OfferSender s, R receiver)
	: s_{std::move(s)}, receiver_{std::move(receiver)} { }

	OfferOperation(const OfferOperation &) = delete;

	OfferOperation &operator= (const OfferOperation &) = delete;

private:
	OfferSender s_;
	R receiver_;
	Worklet worklet_;
};

template<typename R>
inline OfferOperation<R> connect(OfferSender s, R receiver) {
	return {std::move(s), std::move(receiver)};
}

inline async::sender_awaiter<OfferSender, frg::tuple<Error, LaneHandle>>
operator co_await(OfferSender s) {
	return {std::move(s)};
}

//---------------------------------------------------------------------------------------

struct AcceptSender {
	LaneHandle lane;
};

template<typename R>
struct AcceptOperation : private StreamPacket, private StreamNode {
	void start() {
		worklet_.setup([] (Worklet *base) {
			auto self = frg::container_of(base, &AcceptOperation::worklet_);
			async::execution::set_value(self->receiver_,
					frg::make_tuple(self->error(), self->lane()));
		});
		StreamPacket::setup(1, &worklet_);
		StreamNode::setup(kTagAccept, this);

		StreamList list;
		list.push_back(this);
		Stream::transmit(s_.lane, list);
	}

	AcceptOperation(AcceptSender s, R receiver)
	: s_{std::move(s)}, receiver_{std::move(receiver)} { }

	AcceptOperation(const AcceptOperation &) = delete;

	AcceptOperation &operator= (const AcceptOperation &) = delete;

private:
	AcceptSender s_;
	R receiver_;
	Worklet worklet_;
};

template<typename R>
inline AcceptOperation<R> connect(AcceptSender s, R receiver) {
	return {std::move(s), std::move(receiver)};
}

inline async::sender_awaiter<AcceptSender, frg::tuple<Error, LaneHandle>>
operator co_await(AcceptSender s) {
	return {std::move(s)};
}

//---------------------------------------------------------------------------------------

struct ExtractCredentialsSender {
	LaneHandle lane;
};

template<typename R>
struct ExtractCredentialsOperation : private StreamPacket, private StreamNode {
	void start() {
		worklet_.setup([] (Worklet *base) {
			auto self = frg::container_of(base, &ExtractCredentialsOperation::worklet_);
			async::execution::set_value(self->receiver_,
					frg::make_tuple(self->error(), self->credentials()));
		});
		StreamPacket::setup(1, &worklet_);
		StreamNode::setup(kTagExtractCredentials, this);

		StreamList list;
		list.push_back(this);
		Stream::transmit(s_.lane, list);
	}

	ExtractCredentialsOperation(ExtractCredentialsSender s, R receiver)
	: s_{std::move(s)}, receiver_{std::move(receiver)} { }

private:
	ExtractCredentialsSender s_;
	R receiver_;
	Worklet worklet_;
};

template<typename R>
inline ExtractCredentialsOperation<R> connect(ExtractCredentialsSender s, R receiver) {
	return {std::move(s), std::move(receiver)};
}

inline async::sender_awaiter<ExtractCredentialsSender, frg::tuple<Error, frigg::Array<char, 16>>>
operator co_await(ExtractCredentialsSender s) {
	return {std::move(s)};
}

//---------------------------------------------------------------------------------------

struct SendBufferSender {
	LaneHandle lane;
	frigg::UniqueMemory<KernelAlloc> buffer;
};

template<typename R>
struct SendBufferOperation : private StreamPacket, private StreamNode {
	void start() {
		worklet_.setup([] (Worklet *base) {
			auto self = frg::container_of(base, &SendBufferOperation::worklet_);
			async::execution::set_value(self->receiver_, self->error());
		});
		StreamPacket::setup(1, &worklet_);
		StreamNode::setup(kTagSendFromBuffer, this);
		_inBuffer = frigg::move(s_.buffer);

		StreamList list;
		list.push_back(this);
		Stream::transmit(s_.lane, list);
	}

	SendBufferOperation(SendBufferSender s, R receiver)
	: s_{std::move(s)}, receiver_{std::move(receiver)} { }

private:
	SendBufferSender s_;
	R receiver_;
	Worklet worklet_;
};

template<typename R>
inline SendBufferOperation<R> connect(SendBufferSender s, R receiver) {
	return {std::move(s), std::move(receiver)};
}

inline async::sender_awaiter<SendBufferSender, Error>
operator co_await(SendBufferSender s) {
	return {std::move(s)};
}

//---------------------------------------------------------------------------------------

struct RecvBufferSender {
	LaneHandle lane;
};

template<typename R>
struct RecvBufferOperation : private StreamPacket, private StreamNode {
	void start() {
		worklet_.setup([] (Worklet *base) {
			auto self = frg::container_of(base, &RecvBufferOperation::worklet_);
			async::execution::set_value(self->receiver_,
					frg::make_tuple(self->error(), self->transmitBuffer()));
		});
		StreamPacket::setup(1, &worklet_);
		StreamNode::setup(kTagRecvInline, this);
		_maxLength = SIZE_MAX;

		StreamList list;
		list.push_back(this);
		Stream::transmit(s_.lane, list);
	}

	RecvBufferOperation(RecvBufferSender s, R receiver)
	: s_{std::move(s)}, receiver_{std::move(receiver)} { }

private:
	RecvBufferSender s_;
	R receiver_;
	Worklet worklet_;
};

template<typename R>
inline RecvBufferOperation<R> connect(RecvBufferSender s, R receiver) {
	return {std::move(s), std::move(receiver)};
}

inline async::sender_awaiter<RecvBufferSender, frg::tuple<Error, frigg::UniqueMemory<KernelAlloc>>>
operator co_await(RecvBufferSender s) {
	return {std::move(s)};
}

//---------------------------------------------------------------------------------------

struct PushDescriptorSender {
	LaneHandle lane;
	AnyDescriptor descriptor;
};

template<typename R>
struct PushDescriptorOperation : private StreamPacket, private StreamNode {
	void start() {
		worklet_.setup([] (Worklet *base) {
			auto self = frg::container_of(base, &PushDescriptorOperation::worklet_);
			async::execution::set_value(self->receiver_, self->error());
		});
		StreamPacket::setup(1, &worklet_);
		StreamNode::setup(kTagPushDescriptor, this);
		_inDescriptor = std::move(s_.descriptor);

		StreamList list;
		list.push_back(this);
		Stream::transmit(s_.lane, list);
	}

	PushDescriptorOperation(PushDescriptorSender s, R receiver)
	: s_{std::move(s)}, receiver_{std::move(receiver)} { }

private:
	PushDescriptorSender s_;
	R receiver_;
	Worklet worklet_;
};

template<typename R>
inline PushDescriptorOperation<R> connect(PushDescriptorSender s, R receiver) {
	return {std::move(s), std::move(receiver)};
}

inline async::sender_awaiter<PushDescriptorSender, Error>
operator co_await(PushDescriptorSender s) {
	return {std::move(s)};
}

//---------------------------------------------------------------------------------------

struct PullDescriptorSender {
	LaneHandle lane;
};

template<typename R>
struct PullDescriptorOperation : private StreamPacket, private StreamNode {
	void start() {
		worklet_.setup([] (Worklet *base) {
			auto self = frg::container_of(base, &PullDescriptorOperation::worklet_);
			async::execution::set_value(self->receiver_,
					frg::make_tuple(self->error(), self->descriptor()));
		});
		StreamPacket::setup(1, &worklet_);
		StreamNode::setup(kTagPullDescriptor, this);

		StreamList list;
		list.push_back(this);
		Stream::transmit(s_.lane, list);
	}

	PullDescriptorOperation(PullDescriptorSender s, R receiver)
	: s_{std::move(s)}, receiver_{std::move(receiver)} { }

private:
	PullDescriptorSender s_;
	R receiver_;
	Worklet worklet_;
};

template<typename R>
inline PullDescriptorOperation<R> connect(PullDescriptorSender s, R receiver) {
	return {std::move(s), std::move(receiver)};
}

inline async::sender_awaiter<PullDescriptorSender, frg::tuple<Error, AnyDescriptor>>
operator co_await(PullDescriptorSender s) {
	return {std::move(s)};
}

//---------------------------------------------------------------------------------------

// Returns true if an IPC error is caused by the remote side not following the protocol.
inline bool isRemoteIpcError(Error e) {
	switch(e) {
		case Error::bufferTooSmall:
		case Error::transmissionMismatch:
			return true;
		default:
			return false;
	}
}


} // namespace thor
