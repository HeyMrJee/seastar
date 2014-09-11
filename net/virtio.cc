/*
 * Copyright (C) 2014 Cloudius Systems, Ltd.
 */

#include "virtio.hh"
#include "core/posix.hh"
#include "core/vla.hh"
#include "virtio-interface.hh"
#include "core/reactor.hh"
#include <atomic>
#include <vector>
#include <queue>
#include <fcntl.h>
#include <linux/vhost.h>
#include <linux/if.h>
#include <linux/if_tun.h>

using namespace net;

using phys = uint64_t;

template <typename T>
inline
T align_up(T v, T align) {
    return (v + align - 1) & ~(align - 1);
}

template <typename T>
inline
T* align_up(T* v, size_t align) {
    static_assert(sizeof(T) == 1, "align byte pointers only");
    return reinterpret_cast<T*>(align_up(reinterpret_cast<uintptr_t>(v), align));
}

inline
phys virt_to_phys(void* p) {
    return reinterpret_cast<uintptr_t>(p);
}

class vring {
public:
    struct config {
        char* descs;
        char* avail;
        char* used;
        unsigned size;
        bool event_index;
        bool indirect;
        bool mergable_buffers;
    };
    struct buffer {
        phys addr;
        uint32_t len;
        promise<size_t> completed;
        bool writeable;
    };
    using buffer_chain = std::vector<buffer>;
    // provide buffers for the queue, wait on @available to gain buffer space
    using producer_type = future<std::vector<buffer_chain>> (semaphore& available);
private:
    class desc {
    public:
        struct flags {
            // This marks a buffer as continuing via the next field.
            uint16_t has_next : 1;
            // This marks a buffer as write-only (otherwise read-only).
            uint16_t writeable : 1;
            // This means the buffer contains a list of buffer descriptors.
            uint16_t indirect : 1;
        };

        phys get_paddr();
        uint32_t get_len() { return _len; }
        uint16_t next_idx() { return _next; }

        phys _paddr;
        uint32_t _len;
        flags _flags;
        uint16_t _next;
    };

    // Guest to host
    struct avail_layout {
        struct flags {
            // Mark that we do not need an interrupt for consuming a descriptor
            // from the ring. Unreliable so it's simply an optimization
            uint16_t no_interrupts : 1;
        };

        std::atomic<uint16_t> _flags;

        // Where we put the next descriptor
        std::atomic<uint16_t> _idx;
        // There may be no more entries than the queue size read from device
        uint16_t _ring[];
        // used event index is an optimization in order to get an interrupt from the host
        // only when the value reaches this number
        // The location of this field is places after the variable length ring array,
        // that's why we cannot fully define it within the struct and use a function accessor
        //std::atomic<uint16_t> used_event;
    };

    struct used_elem {
        // Index of start of used _desc chain. (uint32_t for padding reasons)
        uint32_t _id;
        // Total length of the descriptor chain which was used (written to)
        uint32_t _len;
    };

    // Host to guest
    struct used_layout {
        enum {
            // The Host advise the Guest: don't kick me when
            // you add a buffer.  It's unreliable, so it's simply an
            // optimization. Guest will still kick if it's out of buffers.
            no_notify = 1
        };

        bool notifications_disabled() {
            return (_flags.load(std::memory_order_relaxed) & VRING_USED_F_NO_NOTIFY) != 0;
        }

        // Using std::atomic since it being changed by the host
        std::atomic<uint16_t> _flags;
        // Using std::atomic in order to have memory barriers for it
        std::atomic<uint16_t> _idx;
        used_elem _used_elements[];
        // avail event index is an optimization kick the host only when the value reaches this number
        // The location of this field is places after the variable length ring array,
        // that's why we cannot fully define it within the struct and use a function accessor
        //std::atomic<uint16_t> avail_event;
    };

    struct avail {
        explicit avail(config conf);
        avail_layout* _shared;
        std::atomic<uint16_t>* _host_notify_on_index = nullptr;
        uint16_t _head = 0;
    };
    struct used {
        explicit used(config conf);
        used_layout* _shared;
        std::atomic<uint16_t>* _notify_on_index = nullptr;
        uint16_t _tail = 0;
    };
private:
    config _config;
    readable_eventfd _notified;
    writeable_eventfd _kick;
    std::function<producer_type> _producer;
    std::unique_ptr<promise<size_t>[]> _completions;
    desc* _descs;
    avail _avail;
    used _used;
    semaphore _available_descriptors = { 0 };
    int _free_desc = -1;
public:
    vring(config conf, readable_eventfd notified, writeable_eventfd kick,
          std::function<producer_type> producer);

    // start the queue
    void run();

    // complete any buffers returned from the host
    void complete();

    // wait for the used ring to have at least @nr buffers
    future<> on_used(size_t nr);

    // Total number of descriptors in ring
    int size() { return _config.size; }

    // Let host know about interrupt delivery
    void disable_interrupts();
    void enable_interrupts();
private:
    size_t mask() { return size() - 1; }
    size_t masked(size_t idx) { return idx & mask(); }
    size_t available();
    unsigned allocate_desc();
    void free_desc(unsigned id);
    void setup();
};

vring::avail::avail(config conf)
    : _shared(reinterpret_cast<avail_layout*>(conf.avail)) {
}

vring::used::used(config conf)
    : _shared(reinterpret_cast<used_layout*>(conf.used)) {
}

inline
unsigned vring::allocate_desc() {
    assert(_free_desc != -1);
    auto desc = _free_desc;
    _free_desc = _descs[desc]._next;
    return desc;
}

inline
void vring::free_desc(unsigned id) {
    _descs[id]._next = _free_desc;
    _free_desc = id;
    _available_descriptors.signal();
}

vring::vring(config conf, readable_eventfd notified, writeable_eventfd kick,
      std::function<producer_type> producer)
    : _config(conf)
    , _notified(std::move(notified))
    , _kick(std::move(kick))
    , _producer(std::move(producer))
    , _completions(new promise<size_t>[_config.size])
    , _descs(reinterpret_cast<desc*>(conf.descs))
    , _avail(conf)
    , _used(conf)
{
    setup();
}

void vring::setup() {
    for (unsigned i = 0; i < _config.size; ++i) {
        free_desc(i);
    }
}

void vring::run() {
    _producer(_available_descriptors).then([this] (std::vector<buffer_chain> vbc) {
        for (auto&& bc: vbc) {
            bool has_prev = false;
            unsigned prev_desc_idx = 0;
            for (auto i = bc.rbegin(); i != bc.rend(); ++i) {
                unsigned desc_idx = allocate_desc();
                desc& d = _descs[desc_idx];
                d._flags = {};
                d._flags.writeable = i->writeable;
                d._flags.has_next = has_prev;
                has_prev = true;
                d._next = prev_desc_idx;
                d._paddr = i->addr;
                d._len = i->len;
                prev_desc_idx = desc_idx;
                _completions[desc_idx] = std::move(i->completed);
            }
            auto desc_head = prev_desc_idx;
            _avail._shared->_ring[masked(_avail._head++)] = desc_head;
        }
        _avail._shared->_idx.store(_avail._head, std::memory_order_release);
        _kick.signal(1);
        complete();
        run();
    });
}

void vring::complete() {
    auto used_head = _used._shared->_idx.load(std::memory_order_acquire);
    while (used_head != _used._tail) {
        auto ue = _used._shared->_used_elements[masked(_used._tail++)];
        _completions[ue._id].set_value(ue._len);
        auto id = ue._id;
        auto has_next = true;
        while (has_next) {
            auto& d = _descs[id];
            auto next = d._next;
            has_next = d._flags.has_next;
            free_desc(id);
            id = next;
        }
    }
    _notified.wait().then([this] (size_t ignore) {
        complete();
    });
}

class virtio_net_device : public net::device {
    struct init {
        readable_eventfd _txq_notify;
        writeable_eventfd _txq_kick;
        readable_eventfd _rxq_notify;
        writeable_eventfd _rxq_kick;
        int _txq_notify_fd;
        int _txq_kick_fd;
        int _rxq_notify_fd;
        int _rxq_kick_fd;
        init() {
            _txq_notify_fd = _txq_notify.get_write_fd();
            _txq_kick_fd = _txq_kick.get_read_fd();
            _rxq_notify_fd = _rxq_notify.get_write_fd();
            _rxq_kick_fd = _txq_kick.get_read_fd();
        }
    };
    struct net_hdr {
        uint8_t needs_csum : 1;
        uint8_t flags_reserved : 7;
        enum { gso_none = 0, gso_tcpv4 = 1, gso_udp = 3, gso_tcpv6 = 4, gso_ecn = 0x80 };
        uint8_t gso_type;
        uint16_t hdr_len;
        uint16_t gso_size;
        uint16_t csum_start;
        uint16_t csum_offset;
    };
    struct net_hdr_mrg : net_hdr {
        uint16_t num_buffers;
    };
    class txq {
        virtio_net_device& _dev;
        vring _ring;
    public:
        txq(virtio_net_device& dev, vring::config config,
                readable_eventfd notified, writeable_eventfd kicked);
        void run() { _ring.run(); }
        future<> post(packet p);
    private:
        future<std::vector<vring::buffer_chain>> transmit(semaphore& available);
        std::queue<packet> _tx_queue;
        semaphore _tx_queue_length = { 0 };
    };
    class rxq  {
        virtio_net_device& _dev;
        vring _ring;
    public:
        rxq(virtio_net_device& _if,
                vring::config config, readable_eventfd notified, writeable_eventfd kicked);
        void run() { _ring.run(); }
    private:
        future<std::vector<vring::buffer_chain>> prepare_buffers(semaphore& available);
        void received(char* buffer);
    };
private:
    size_t _header_len = sizeof(net_hdr); // adjust for mrg_buf
    file_desc _tap_fd;
    file_desc _vhost_fd;
    std::unique_ptr<char[], free_deleter> _txq_storage;
    std::unique_ptr<char[], free_deleter> _rxq_storage;
    txq _txq;
    rxq _rxq;
    semaphore _rx_queue_length = { 0 };
    std::queue<packet> _rx_queue;
private:
    vring::config txq_config();
    vring::config rxq_config();
    void queue_rx_packet(packet p);
public:
    explicit virtio_net_device(sstring tap_device, init x = init());
    virtual future<packet> receive() override;
    virtual future<> send(packet p) override;
    virtual ethernet_address hw_address() override;
};

virtio_net_device::txq::txq(virtio_net_device& dev, vring::config config,
        readable_eventfd notified, writeable_eventfd kicked)
    : _dev(dev), _ring(config, std::move(notified), std::move(kicked),
            [this] (semaphore& available) { return transmit(available); }) {
}

future<std::vector<vring::buffer_chain>>
virtio_net_device::txq::transmit(semaphore& available) {
    return _tx_queue_length.wait().then([this, &available] {
        auto p = std::move(_tx_queue.front());
        _tx_queue.pop();
        // Linux requires that hdr_len be sane even if gso is disabled.
        net_hdr_mrg vhdr = {};
        // prepend virtio-net header
        packet q = packet(fragment{reinterpret_cast<char*>(&vhdr), _dev._header_len},
                std::move(p));
        auto nbufs = q.fragments.size();
        return available.wait(nbufs).then([this, p = std::move(q)] () mutable {
            std::vector<vring::buffer_chain> vbc;
            vring::buffer_chain bc;
            for (auto&& f : p.fragments) {
                vring::buffer b;
                b.addr = virt_to_phys(f.base);
                b.len = f.size;
                b.writeable = false;
                bc.push_back(std::move(b));
            }
            // schedule packet destruction
            bc[0].completed.get_future().then([p = std::move(p)] (size_t) {});
            vbc.push_back(std::move(bc));
            return make_ready_future<std::vector<vring::buffer_chain>>(std::move(vbc));
        });
    });
}

future<>
virtio_net_device::txq::post(packet p) {
    _tx_queue.push(std::move(p));
    _tx_queue_length.signal();
    return make_ready_future<>(); // FIXME: queue bounds
}

virtio_net_device::rxq::rxq(virtio_net_device& netif,
        vring::config config, readable_eventfd notified, writeable_eventfd kicked)
    : _dev(netif), _ring(config, std::move(notified), std::move(kicked),
            [this] (semaphore& available) { return prepare_buffers(available); }) {
}

future<std::vector<vring::buffer_chain>>
virtio_net_device::rxq::prepare_buffers(semaphore& available) {
    return available.wait(1).then([this, &available] {
        unsigned count = 1;
        auto opportunistic = available.current();
        if (available.try_wait(opportunistic)) {
            count += opportunistic;
        }
        std::vector<vring::buffer_chain> ret;
        ret.reserve(count);
        for (unsigned i = 0; i < count; ++i) {
            vring::buffer_chain bc;
            std::unique_ptr<char[]> buf(new char[4096]);
            vring::buffer b;
            b.addr = virt_to_phys(buf.get());
            b.len = 4096;
            b.writeable = true;
            b.completed.get_future().then([this, buf = buf.get()] (size_t len) {
                packet p(fragment{buf + _dev._header_len, len - _dev._header_len},
                        [buf] { delete[] buf; });
                _dev.queue_rx_packet(std::move(p));
            });
            bc.push_back(std::move(b));
            buf.release();
            ret.push_back(std::move(bc));
        }
        return make_ready_future<std::vector<vring::buffer_chain>>(std::move(ret));
    });
}

virtio_net_device::virtio_net_device(sstring tap_device, init x)
    : _tap_fd(file_desc::open("/dev/net/tun", O_RDWR | O_NONBLOCK))
    , _vhost_fd(file_desc::open("/dev/vhost-net", O_RDWR))
    , _txq_storage(allocate_aligned_buffer<char>(3*4096, 4096))
    , _rxq_storage(allocate_aligned_buffer<char>(3*4096, 4096))
    , _txq(*this, txq_config(), std::move(x._txq_notify), std::move(x._txq_kick))
    , _rxq(*this, rxq_config(), std::move(x._rxq_notify), std::move(x._rxq_kick)) {
    assert(tap_device.size() + 1 <= IFNAMSIZ);
    ifreq ifr = {};
    ifr.ifr_flags = IFF_TAP | IFF_NO_PI | IFF_ONE_QUEUE | IFF_VNET_HDR;
    strcpy(ifr.ifr_ifrn.ifrn_name, tap_device.c_str());
    _tap_fd.ioctl(TUNSETIFF, ifr);
    _vhost_fd.ioctl(VHOST_SET_OWNER);
    auto mem_table = make_struct_with_vla(&vhost_memory::regions, 1);
    mem_table->nregions = 1;
    auto& region = mem_table->regions[0];
    region.guest_phys_addr = 0;
    region.memory_size = (size_t(1) << 47) - 4096;
    region.userspace_addr = 0;
    region.flags_padding = 0;
    _vhost_fd.ioctl(VHOST_SET_MEM_TABLE, *mem_table);
    uint64_t features =
            /* VIRTIO_RING_F_EVENT_IDX
            | */ VIRTIO_RING_F_INDIRECT_DESC
            /* | VIRTIO_NET_F_MRG_RXBUF */;
    _vhost_fd.ioctl(VHOST_SET_FEATURES, features);
    vhost_vring_state vvs0 = { 0, 256 };
    _vhost_fd.ioctl(VHOST_SET_VRING_NUM, vvs0);
    vhost_vring_state vvs1 = { 1, 256 };
    _vhost_fd.ioctl(VHOST_SET_VRING_NUM, vvs1);
    auto tov = [](char* x) { return reinterpret_cast<uintptr_t>(x); };

    _vhost_fd.ioctl(VHOST_SET_VRING_ADDR, vhost_vring_addr{
            0, 0, tov(rxq_config().descs), tov(rxq_config().used), tov(rxq_config().avail), 0
    });
    _vhost_fd.ioctl(VHOST_SET_VRING_ADDR, vhost_vring_addr{
            1, 0, tov(txq_config().descs), tov(txq_config().used), tov(txq_config().avail), 0
    });
    _vhost_fd.ioctl(VHOST_SET_VRING_KICK, vhost_vring_file{0, x._rxq_kick_fd});
    _vhost_fd.ioctl(VHOST_SET_VRING_CALL, vhost_vring_file{0, x._rxq_notify_fd});
    _vhost_fd.ioctl(VHOST_SET_VRING_KICK, vhost_vring_file{1, x._txq_kick_fd});
    _vhost_fd.ioctl(VHOST_SET_VRING_CALL, vhost_vring_file{1, x._txq_notify_fd});
    _vhost_fd.ioctl(VHOST_NET_SET_BACKEND, vhost_vring_file{0, _tap_fd.get()});
    _vhost_fd.ioctl(VHOST_NET_SET_BACKEND, vhost_vring_file{1, _tap_fd.get()});
    _txq.run();
    _rxq.run();
}

vring::config virtio_net_device::txq_config() {
    vring::config r;
    auto size = 256;
    r.descs = _txq_storage.get();
    r.avail = r.descs + 16 * size;
    r.used = align_up(r.avail + 2 * size + 6, 4096);
    r.size = size;
    r.event_index = !true;
    r.indirect = false;
    r.mergable_buffers = false;
    return r;
}

vring::config virtio_net_device::rxq_config() {
    vring::config r;
    auto size = 256;
    r.descs = _rxq_storage.get();
    r.avail = r.descs + 16 * size;
    r.used = align_up(r.avail + 2 * size + 6, 4096);
    r.size = size;
    r.event_index = !true;
    r.indirect = false;
    r.mergable_buffers = true;
    return r;
}

future<packet>
virtio_net_device::receive() {
    return _rx_queue_length.wait().then([this] {
        auto p = std::move(_rx_queue.front());
        _rx_queue.pop();
        return make_ready_future<packet>(std::move(p));
    });
}

future<>
virtio_net_device::send(packet p) {
    return _txq.post(std::move(p));
}

void virtio_net_device::queue_rx_packet(packet p) {
    _rx_queue.push(std::move(p));
    _rx_queue_length.signal(1);
}

ethernet_address virtio_net_device::hw_address() {
    return { 0x12, 0x23, 0x34, 0x56, 0x67, 0x78 };
}

std::unique_ptr<net::device> create_virtio_net_device(sstring tap_device) {
    return std::make_unique<virtio_net_device>(tap_device);
}