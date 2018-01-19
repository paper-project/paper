#pragma once

#include <paper/secure.hpp>

#include <unordered_set>
#include <memory>
#include <queue>
#include <mutex>
#include <condition_variable>
#include <thread>
#include <atomic>

#include <boost/asio.hpp>
#include <boost/network/include/http/server.hpp>
#include <boost/network/utils/thread_pool.hpp>
#include <boost/iostreams/device/array.hpp>
#include <boost/log/trivial.hpp>
#include <boost/log/sources/logger.hpp>
#include <boost/multi_index_container.hpp>
#include <boost/multi_index/ordered_index.hpp>
#include <boost/multi_index/hashed_index.hpp>
#include <boost/multi_index/member.hpp>
#include <boost/multi_index/random_access_index.hpp>
#include <boost/circular_buffer.hpp>

#include <xxhash/xxhash.h>

std::ostream & operator << (std::ostream &, std::chrono::system_clock::time_point const &);
namespace paper
{
using endpoint = boost::asio::ip::udp::endpoint;
using tcp_endpoint = boost::asio::ip::tcp::endpoint;
bool parse_endpoint (std::string const &, paper::endpoint &);
bool parse_tcp_endpoint (std::string const &, paper::tcp_endpoint &);
bool reserved_address (paper::endpoint const &);
}

static uint64_t endpoint_hash_raw (paper::endpoint const & endpoint_a)
{
	assert (endpoint_a.address ().is_v6 ());
	paper::uint128_union address;
	address.bytes = endpoint_a.address ().to_v6 ().to_bytes ();
	XXH64_state_t hash;
	XXH64_reset (&hash, 0);
	XXH64_update (&hash, address.bytes.data (), address.bytes.size ());
	auto port (endpoint_a.port ());
	XXH64_update (&hash, &port, sizeof (port));
	auto result (XXH64_digest (&hash));
	return result;
}

namespace std
{
template <size_t size>
struct endpoint_hash
{
};
template <>
struct endpoint_hash <8>
{
    size_t operator () (paper::endpoint const & endpoint_a) const
    {
		return endpoint_hash_raw (endpoint_a);
    }
};
template <>
struct endpoint_hash <4>
{
    size_t operator () (paper::endpoint const & endpoint_a) const
    {
		uint64_t big (endpoint_hash_raw (endpoint_a));
		uint32_t result (static_cast <uint32_t> (big) ^ static_cast <uint32_t> (big >> 32));
		return result;
    }
};
template <>
struct hash <paper::endpoint>
{
    size_t operator () (paper::endpoint const & endpoint_a) const
    {
        endpoint_hash <sizeof (size_t)> ehash;
        return ehash (endpoint_a);
    }
};
}
namespace boost
{
template <>
struct hash <paper::endpoint>
{
    size_t operator () (paper::endpoint const & endpoint_a) const
    {
        std::hash <paper::endpoint> hash;
        return hash (endpoint_a);
    }
};
}

namespace paper
{
class node;
class election : public std::enable_shared_from_this <paper::election>
{
public:
    election (std::shared_ptr <paper::node>, paper::block const &, std::function <void (paper::block &)> const &);
    void start ();
    void vote (paper::vote const &);
    void announce_vote ();
    void timeout_action ();
    void start_request (paper::block const &);
    paper::uint128_t uncontested_threshold (MDB_txn *, paper::ledger &);
    paper::uint128_t contested_threshold (MDB_txn *, paper::ledger &);
    paper::votes votes;
    std::weak_ptr <paper::node> node;
    std::chrono::system_clock::time_point last_vote;
	std::unique_ptr <paper::block> last_winner;
    bool confirmed;
	std::function <void (paper::block &)> confirmation_action;
};
class conflicts
{
public:
    conflicts (paper::node &);
    void start (paper::block const &, std::function <void (paper::block &)> const &, bool);
    bool no_conflict (paper::block_hash const &);
    void update (paper::vote const &);
    void stop (paper::block_hash const &);
    std::unordered_map <paper::block_hash, std::shared_ptr <paper::election>> roots;
    paper::node & node;
    std::mutex mutex;
};
enum class message_type : uint8_t
{
    invalid,
    not_a_type,
    keepalive,
    publish,
    confirm_req,
    confirm_ack,
    bulk_pull,
    bulk_push,
    frontier_req
};
class message_visitor;
class message
{
public:
    message (paper::message_type);
	message (bool &, paper::stream &);
    virtual ~message () = default;
    void write_header (paper::stream &);
    static bool read_header (paper::stream &, uint8_t &, uint8_t &, uint8_t &, paper::message_type &, std::bitset <16> &);
    virtual void serialize (paper::stream &) = 0;
    virtual bool deserialize (paper::stream &) = 0;
    virtual void visit (paper::message_visitor &) const = 0;
    paper::block_type block_type () const;
    void block_type_set (paper::block_type);
    bool ipv4_only ();
    void ipv4_only_set (bool);
    static std::array <uint8_t, 2> constexpr magic_number = {{'R', paper::paper_network == paper::paper_networks::paper_test_network ? 'A' : paper::paper_network == paper::paper_networks::paper_beta_network ? 'B' : 'C'}};
    uint8_t version_max;
    uint8_t version_using;
    uint8_t version_min;
    paper::message_type type;
    std::bitset <16> extensions;
    static size_t constexpr ipv4_only_position = 1;
    static size_t constexpr bootstrap_server_position = 2;
    static std::bitset <16> constexpr block_type_mask = std::bitset <16> (0x0f00);
};
class keepalive : public message
{
public:
    keepalive ();
    void visit (paper::message_visitor &) const override;
    bool deserialize (paper::stream &) override;
    void serialize (paper::stream &) override;
    bool operator == (paper::keepalive const &) const;
    std::array <paper::endpoint, 8> peers;
};
class publish : public message
{
public:
    publish ();
    publish (std::unique_ptr <paper::block>);
    void visit (paper::message_visitor &) const override;
    bool deserialize (paper::stream &) override;
    void serialize (paper::stream &) override;
    bool operator == (paper::publish const &) const;
    std::unique_ptr <paper::block> block;
};
class confirm_req : public message
{
public:
    confirm_req ();
    confirm_req (std::unique_ptr <paper::block>);
    bool deserialize (paper::stream &) override;
    void serialize (paper::stream &) override;
    void visit (paper::message_visitor &) const override;
    bool operator == (paper::confirm_req const &) const;
    std::unique_ptr <paper::block> block;
};
class confirm_ack : public message
{
public:
	confirm_ack (bool &, paper::stream &);
    confirm_ack (paper::account const &, paper::private_key const &, uint64_t, std::unique_ptr <paper::block>);
    bool deserialize (paper::stream &) override;
    void serialize (paper::stream &) override;
    void visit (paper::message_visitor &) const override;
    bool operator == (paper::confirm_ack const &) const;
    paper::vote vote;
};
class frontier_req : public message
{
public:
    frontier_req ();
    bool deserialize (paper::stream &) override;
    void serialize (paper::stream &) override;
    void visit (paper::message_visitor &) const override;
    bool operator == (paper::frontier_req const &) const;
    paper::account start;
    uint32_t age;
    uint32_t count;
};
class bulk_pull : public message
{
public:
    bulk_pull ();
    bool deserialize (paper::stream &) override;
    void serialize (paper::stream &) override;
    void visit (paper::message_visitor &) const override;
    paper::uint256_union start;
    paper::block_hash end;
    uint32_t count;
};
class bulk_push : public message
{
public:
    bulk_push ();
    bool deserialize (paper::stream &) override;
    void serialize (paper::stream &) override;
    void visit (paper::message_visitor &) const override;
};
class message_visitor
{
public:
    virtual void keepalive (paper::keepalive const &) = 0;
    virtual void publish (paper::publish const &) = 0;
    virtual void confirm_req (paper::confirm_req const &) = 0;
    virtual void confirm_ack (paper::confirm_ack const &) = 0;
    virtual void bulk_pull (paper::bulk_pull const &) = 0;
    virtual void bulk_push (paper::bulk_push const &) = 0;
    virtual void frontier_req (paper::frontier_req const &) = 0;
};
// The fan spreads a key out over the heap to decrease the likelyhood of it being recovered by memory inspection
class fan
{
public:
    fan (paper::uint256_union const &, size_t);
    paper::uint256_union value ();
    void value_set (paper::uint256_union const &);
    std::vector <std::unique_ptr <paper::uint256_union>> values;
};
class wallet_value
{
public:
	wallet_value () = default;
	wallet_value (MDB_val const &);
	wallet_value (paper::uint256_union const &);
	paper::mdb_val val () const;
	paper::private_key key;
	uint64_t work;
};
class wallet_store
{
public:
    wallet_store (bool &, paper::transaction &, std::string const &);
    wallet_store (bool &, paper::transaction &, std::string const &, std::string const &);
	std::vector <paper::account> accounts (MDB_txn *);
    void initialize (MDB_txn *, bool &, std::string const &);
    paper::uint256_union check (MDB_txn *);
    bool rekey (MDB_txn *, std::string const &);
    bool valid_password (MDB_txn *);
    void enter_password (MDB_txn *, std::string const &);
    paper::uint256_union wallet_key (MDB_txn *);
    paper::uint256_union salt (MDB_txn *);
    bool is_representative (MDB_txn *);
    paper::account representative (MDB_txn *);
    void representative_set (MDB_txn *, paper::account const &);
    paper::public_key insert (MDB_txn *, paper::private_key const &);
    void erase (MDB_txn *, paper::public_key const &);
	paper::wallet_value entry_get_raw (MDB_txn *, paper::public_key const &);
	void entry_put_raw (MDB_txn *, paper::public_key const &, paper::wallet_value const &);
    bool fetch (MDB_txn *, paper::public_key const &, paper::private_key &);
    bool exists (MDB_txn *, paper::public_key const &);
	void destroy (MDB_txn *);
    paper::store_iterator find (MDB_txn *, paper::uint256_union const &);
    paper::store_iterator begin (MDB_txn *);
    paper::store_iterator end ();
    paper::uint256_union derive_key (MDB_txn *, std::string const &);
    void serialize_json (MDB_txn *, std::string &);
	void write_backup (MDB_txn *, boost::filesystem::path const &);
    bool move (MDB_txn *, paper::wallet_store &, std::vector <paper::public_key> const &);
	bool import (MDB_txn *, paper::wallet_store &);
	bool work_get (MDB_txn *, paper::public_key const &, uint64_t &);
	void work_put (MDB_txn *, paper::public_key const &, uint64_t);
    paper::fan password;
    static paper::uint256_union const version_1;
    static paper::uint256_union const version_current;
    static paper::uint256_union const version_special;
    static paper::uint256_union const wallet_key_special;
    static paper::uint256_union const salt_special;
    static paper::uint256_union const check_special;
    static paper::uint256_union const representative_special;
    static int const special_count;
    static size_t const kdf_full_work = 8 * 1024 * 1024; // 8 * 8 * 1024 * 1024 = 64 MB memory to derive key
    static size_t const kdf_test_work = 1024;
    static size_t const kdf_work = paper::paper_network == paper::paper_networks::paper_test_network ? kdf_test_work : kdf_full_work;
	paper::mdb_env & environment;
    MDB_dbi handle;
};
// A wallet is a set of account keys encrypted by a common encryption key
class wallet : public std::enable_shared_from_this <paper::wallet>
{
public:
    wallet (bool &, paper::transaction &, paper::node &, std::string const &);
    wallet (bool &, paper::transaction &, paper::node &, std::string const &, std::string const &);
	void enter_initial_password (MDB_txn *);
	paper::public_key insert (paper::private_key const &);
    bool exists (paper::public_key const &);
	bool import (std::string const &, std::string const &);
	void serialize (std::string &);
	bool change_action (paper::account const &, paper::account const &);
    bool receive_action (paper::send_block const &, paper::private_key const &, paper::account const &);
	bool send_action (paper::account const &, paper::account const &, paper::uint128_t const &);
	bool change_sync (paper::account const &, paper::account const &);
    bool receive_sync (paper::send_block const &, paper::private_key const &, paper::account const &);
	bool send_sync (paper::account const &, paper::account const &, paper::uint128_t const &);
    void work_generate (paper::account const &, paper::block_hash const &);
    void work_update (MDB_txn *, paper::account const &, paper::block_hash const &, uint64_t);
    uint64_t work_fetch (MDB_txn *, paper::account const &, paper::block_hash const &);
	bool search_pending ();
    paper::wallet_store store;
    paper::node & node;
};
// The wallets set is all the wallets a node controls.  A node may contain multiple wallets independently encrypted and operated.
class wallets
{
public:
	wallets (bool &, paper::node &);
	std::shared_ptr <paper::wallet> open (paper::uint256_union const &);
	std::shared_ptr <paper::wallet> create (paper::uint256_union const &);
    bool search_pending (paper::uint256_union const &);
	void destroy (paper::uint256_union const &);
	void queue_wallet_action (paper::account const &, std::function <void ()> const &);
	void foreach_representative (std::function <void (paper::public_key const &, paper::private_key const &)> const &);
	std::unordered_map <paper::uint256_union, std::shared_ptr <paper::wallet>> items;
	std::unordered_multimap <paper::account, std::function <void ()>> pending_actions;
	std::unordered_set <paper::account> current_actions;
	std::mutex action_mutex;
	MDB_dbi handle;
	paper::node & node;
};
class operation
{
public:
    bool operator > (paper::operation const &) const;
    std::chrono::system_clock::time_point wakeup;
    std::function <void ()> function;
};
class processor_service
{
public:
    processor_service ();
    void run ();
    size_t poll ();
    size_t poll_one ();
    void add (std::chrono::system_clock::time_point const &, std::function <void ()> const &);
    void stop ();
    bool stopped ();
    size_t size ();
    bool done;
    std::mutex mutex;
    std::condition_variable condition;
    std::priority_queue <operation, std::vector <operation>, std::greater <operation>> operations;
};
class gap_information
{
public:
    std::chrono::system_clock::time_point arrival;
    paper::block_hash required;
    paper::block_hash hash;
	std::unique_ptr <paper::votes> votes;
    std::unique_ptr <paper::block> block;
};
class gap_cache
{
public:
    gap_cache (paper::node &);
    void add (paper::block const &, paper::block_hash);
    std::unique_ptr <paper::block> get (paper::block_hash const &);
    void vote (MDB_txn *, paper::vote const &);
    paper::uint128_t bootstrap_threshold (MDB_txn *);
    boost::multi_index_container
    <
        paper::gap_information,
        boost::multi_index::indexed_by
        <
            boost::multi_index::hashed_unique <boost::multi_index::member <gap_information, paper::block_hash, &gap_information::required>>,
            boost::multi_index::ordered_non_unique <boost::multi_index::member <gap_information, std::chrono::system_clock::time_point, &gap_information::arrival>>,
            boost::multi_index::hashed_unique <boost::multi_index::member <gap_information, paper::block_hash, &gap_information::hash>>
        >
    > blocks;
    size_t const max = 128;
    std::mutex mutex;
    paper::node & node;
};
class block_synchronization
{
public:
    block_synchronization (std::function <void (paper::block const &)> const &, paper::block_store &);
    ~block_synchronization ();
    // Return true if target already has block
    virtual bool synchronized (paper::block_hash const &) = 0;
    virtual std::unique_ptr <paper::block> retrieve (paper::block_hash const &) = 0;
    // return true if all dependencies are synchronized
    bool add_dependency (paper::block const &);
    bool fill_dependencies ();
    bool synchronize_one ();
    bool synchronize (paper::block_hash const &);
    std::stack <paper::block_hash> blocks;
    std::unordered_set <paper::block_hash> sent;
    std::function <void (paper::block const &)> target;
    paper::block_store & store;
};
class pull_synchronization : public paper::block_synchronization
{
public:
    pull_synchronization (std::function <void (paper::block const &)> const &, paper::block_store &);
    bool synchronized (paper::block_hash const &) override;
    std::unique_ptr <paper::block> retrieve (paper::block_hash const &) override;
};
class push_synchronization : public paper::block_synchronization
{
public:
    push_synchronization (std::function <void (paper::block const &)> const &, paper::block_store &);
    bool synchronized (paper::block_hash const &) override;
    std::unique_ptr <paper::block> retrieve (paper::block_hash const &) override;
};
class bootstrap_client : public std::enable_shared_from_this <bootstrap_client>
{
public:
	bootstrap_client (std::shared_ptr <paper::node>, std::function <void ()> const & = [] () {});
    ~bootstrap_client ();
    void run (paper::tcp_endpoint const &);
    void connect_action ();
    void sent_request (boost::system::error_code const &, size_t);
    std::shared_ptr <paper::node> node;
    boost::asio::ip::tcp::socket socket;
	std::function <void ()> completion_action;
};
class frontier_req_client : public std::enable_shared_from_this <paper::frontier_req_client>
{
public:
    frontier_req_client (std::shared_ptr <paper::bootstrap_client> const &);
    ~frontier_req_client ();
    void receive_frontier ();
    void received_frontier (boost::system::error_code const &, size_t);
    void request_account (paper::account const &);
	void unsynced (MDB_txn *, paper::account const &, paper::block_hash const &);
    void completed_requests ();
    void completed_pulls ();
    void completed_pushes ();
	void next ();
    std::unordered_map <paper::account, paper::block_hash> pulls;
    std::array <uint8_t, 200> receive_buffer;
    std::shared_ptr <paper::bootstrap_client> connection;
	paper::account current;
	paper::account_info info;
};
class bulk_pull_client : public std::enable_shared_from_this <paper::bulk_pull_client>
{
public:
    bulk_pull_client (std::shared_ptr <paper::frontier_req_client> const &);
    ~bulk_pull_client ();
    void request ();
    void receive_block ();
    void received_type ();
    void received_block (boost::system::error_code const &, size_t);
    void process_end ();
	paper::block_hash first ();
    std::array <uint8_t, 200> receive_buffer;
    std::shared_ptr <paper::frontier_req_client> connection;
    std::unordered_map <paper::account, paper::block_hash>::iterator current;
    std::unordered_map <paper::account, paper::block_hash>::iterator end;
};
class bulk_push_client : public std::enable_shared_from_this <paper::bulk_push_client>
{
public:
    bulk_push_client (std::shared_ptr <paper::frontier_req_client> const &);
    ~bulk_push_client ();
    void start ();
    void push ();
    void push_block (paper::block const &);
    void send_finished ();
    std::shared_ptr <paper::frontier_req_client> connection;
    paper::push_synchronization synchronization;
};
class message_parser
{
public:
    message_parser (paper::message_visitor &);
    void deserialize_buffer (uint8_t const *, size_t);
    void deserialize_keepalive (uint8_t const *, size_t);
    void deserialize_publish (uint8_t const *, size_t);
    void deserialize_confirm_req (uint8_t const *, size_t);
    void deserialize_confirm_ack (uint8_t const *, size_t);
    bool at_end (paper::bufferstream &);
    paper::message_visitor & visitor;
    bool error;
    bool insufficient_work;
};
class peer_information
{
public:
	paper::endpoint endpoint;
	std::chrono::system_clock::time_point last_contact;
	std::chrono::system_clock::time_point last_attempt;
	std::chrono::system_clock::time_point last_bootstrap_failure;
	paper::block_hash most_recent;
};
class peer_container
{
public:
	peer_container (paper::endpoint const &);
	// We were contacted by endpoint, update peers
    void contacted (paper::endpoint const &);
	// Unassigned, reserved, self
	bool not_a_peer (paper::endpoint const &);
	// Returns true if peer was already known
	bool known_peer (paper::endpoint const &);
	// Notify of peer we received from
	bool insert (paper::endpoint const &);
	// Received from a peer and contained a block announcement
	bool insert (paper::endpoint const &, paper::block_hash const &);
	// Does this peer probably know about this block
	bool knows_about (paper::endpoint const &, paper::block_hash const &);
	// Notify of bootstrap failure
	void bootstrap_failed (paper::endpoint const &);
	void random_fill (std::array <paper::endpoint, 8> &);
	// List of all peers
	std::vector <peer_information> list ();
	// List of peers that haven't failed bootstrapping in a while
	std::vector <peer_information> bootstrap_candidates ();
	// Purge any peer where last_contact < time_point and return what was left
	std::vector <paper::peer_information> purge_list (std::chrono::system_clock::time_point const &);
	size_t size ();
	bool empty ();
	std::mutex mutex;
	paper::endpoint self;
	boost::multi_index_container
	<
		peer_information,
		boost::multi_index::indexed_by
		<
			boost::multi_index::hashed_unique <boost::multi_index::member <peer_information, paper::endpoint, &peer_information::endpoint>>,
			boost::multi_index::ordered_non_unique <boost::multi_index::member <peer_information, std::chrono::system_clock::time_point, &peer_information::last_contact>>,
			boost::multi_index::ordered_non_unique <boost::multi_index::member <peer_information, std::chrono::system_clock::time_point, &peer_information::last_attempt>, std::greater <std::chrono::system_clock::time_point>>
		>
	> peers;
	std::function <void (paper::endpoint const &)> peer_observer;
	std::function <void ()> disconnect_observer;
};
class send_info
{
public:
	uint8_t const * data;
	size_t size;
	paper::endpoint endpoint;
	size_t rebroadcast;
	std::function <void (boost::system::error_code const &, size_t)> callback;
};
class network
{
public:
    network (boost::asio::io_service &, uint16_t, paper::node &);
    void receive ();
    void stop ();
    void receive_action (boost::system::error_code const &, size_t);
    void rpc_action (boost::system::error_code const &, size_t);
    void republish_block (std::unique_ptr <paper::block>, size_t);
    void publish_broadcast (std::vector <paper::peer_information> &, std::unique_ptr <paper::block>);
    bool confirm_broadcast (std::vector <paper::peer_information> &, std::unique_ptr <paper::block>, uint64_t, size_t);
	void confirm_block (paper::private_key const &, paper::public_key const &, std::unique_ptr <paper::block>, uint64_t, paper::endpoint const &, size_t);
    void merge_peers (std::array <paper::endpoint, 8> const &);
    void send_keepalive (paper::endpoint const &);
	void broadcast_confirm_req (paper::block const &);
    void send_confirm_req (paper::endpoint const &, paper::block const &);
	void initiate_send ();
    void send_buffer (uint8_t const *, size_t, paper::endpoint const &, size_t, std::function <void (boost::system::error_code const &, size_t)>);
    void send_complete (boost::system::error_code const &, size_t);
    paper::endpoint endpoint ();
    paper::endpoint remote;
    std::array <uint8_t, 512> buffer;
    boost::asio::ip::udp::socket socket;
    std::mutex socket_mutex;
    boost::asio::io_service & service;
    boost::asio::ip::udp::resolver resolver;
    paper::node & node;
    uint64_t bad_sender_count;
    std::queue <paper::send_info> sends;
    bool on;
    uint64_t keepalive_count;
    uint64_t publish_count;
    uint64_t confirm_req_count;
    uint64_t confirm_ack_count;
    uint64_t insufficient_work_count;
    uint64_t error_count;
    static uint16_t const node_port = paper::paper_network == paper::paper_networks::paper_live_network ? 7075 : 54000;
    static uint16_t const rpc_port = paper::paper_network == paper::paper_networks::paper_live_network ? 7076 : 55000;
};
class bootstrap_initiator
{
public:
	bootstrap_initiator (paper::node &);
	void warmup (paper::endpoint const &);
	void bootstrap (paper::endpoint const &);
    void bootstrap_any ();
	void initiate (paper::endpoint const &);
	void notify_listeners ();
	std::vector <std::function <void (bool)>> observers;
	std::mutex mutex;
	paper::node & node;
	bool in_progress;
	std::unordered_set <paper::endpoint> warmed_up;
};
class bootstrap_listener
{
public:
    bootstrap_listener (boost::asio::io_service &, uint16_t, paper::node &);
    void start ();
    void stop ();
    void accept_connection ();
    void accept_action (boost::system::error_code const &, std::shared_ptr <boost::asio::ip::tcp::socket>);
    paper::tcp_endpoint endpoint ();
    boost::asio::ip::tcp::acceptor acceptor;
    paper::tcp_endpoint local;
    boost::asio::io_service & service;
    paper::node & node;
    bool on;
};
class bootstrap_server : public std::enable_shared_from_this <paper::bootstrap_server>
{
public:
    bootstrap_server (std::shared_ptr <boost::asio::ip::tcp::socket>, std::shared_ptr <paper::node>);
    ~bootstrap_server ();
    void receive ();
    void receive_header_action (boost::system::error_code const &, size_t);
    void receive_bulk_pull_action (boost::system::error_code const &, size_t);
    void receive_frontier_req_action (boost::system::error_code const &, size_t);
    void receive_bulk_push_action ();
    void add_request (std::unique_ptr <paper::message>);
    void finish_request ();
    void run_next ();
    std::array <uint8_t, 128> receive_buffer;
    std::shared_ptr <boost::asio::ip::tcp::socket> socket;
    std::shared_ptr <paper::node> node;
    std::mutex mutex;
    std::queue <std::unique_ptr <paper::message>> requests;
};
class bulk_pull_server : public std::enable_shared_from_this <paper::bulk_pull_server>
{
public:
    bulk_pull_server (std::shared_ptr <paper::bootstrap_server> const &, std::unique_ptr <paper::bulk_pull>);
    void set_current_end ();
    std::unique_ptr <paper::block> get_next ();
    void send_next ();
    void sent_action (boost::system::error_code const &, size_t);
    void send_finished ();
    void no_block_sent (boost::system::error_code const &, size_t);
    std::shared_ptr <paper::bootstrap_server> connection;
    std::unique_ptr <paper::bulk_pull> request;
    std::vector <uint8_t> send_buffer;
    paper::block_hash current;
};
class bulk_push_server : public std::enable_shared_from_this <paper::bulk_push_server>
{
public:
    bulk_push_server (std::shared_ptr <paper::bootstrap_server> const &);
    void receive ();
    void receive_block ();
    void received_type ();
    void received_block (boost::system::error_code const &, size_t);
    void process_end ();
    std::array <uint8_t, 256> receive_buffer;
    std::shared_ptr <paper::bootstrap_server> connection;
};
class frontier_req_server : public std::enable_shared_from_this <paper::frontier_req_server>
{
public:
    frontier_req_server (std::shared_ptr <paper::bootstrap_server> const &, std::unique_ptr <paper::frontier_req>);
    void skip_old ();
    void send_next ();
    void sent_action (boost::system::error_code const &, size_t);
    void send_finished ();
    void no_block_sent (boost::system::error_code const &, size_t);
	void next ();
    std::shared_ptr <paper::bootstrap_server> connection;
	paper::account current;
	paper::account_info info;
    std::unique_ptr <paper::frontier_req> request;
    std::vector <uint8_t> send_buffer;
    size_t count;
};
class rpc_config
{
public:
	rpc_config ();
	rpc_config (bool);
    void serialize_json (boost::property_tree::ptree &) const;
	bool deserialize_json (boost::property_tree::ptree const &);
	boost::asio::ip::address_v6 address;
	uint16_t port;
	bool enable_control;
};
class rpc
{
public:
    rpc (boost::shared_ptr <boost::asio::io_service>, boost::shared_ptr <boost::network::utils::thread_pool>, paper::node &, paper::rpc_config const &);
    void start ();
    void stop ();
    void operator () (boost::network::http::server <paper::rpc>::request const &, boost::network::http::server <paper::rpc>::response &);
    void log (const char *) {}
	paper::rpc_config config;
    boost::network::http::server <paper::rpc> server;
    paper::node & node;
    bool on;
};
class work_pool
{
public:
	work_pool ();
	~work_pool ();
	void loop (uint64_t);
	void stop ();
	uint64_t generate (paper::uint256_union const &);
	void generate (paper::block &);
	paper::uint256_union current;
	std::atomic <int> ticket;
	bool done;
	std::vector <std::thread> threads;
	std::unordered_map <paper::uint256_union, uint64_t> completed;
	std::queue <paper::uint256_union> pending;
	std::mutex mutex;
	std::condition_variable consumer_condition;
	std::condition_variable producer_condition;
};
class kdf
{
public:
	kdf (size_t);
	paper::uint256_union generate (std::string const &, paper::uint256_union const &);
	size_t const entries;
	std::unique_ptr <uint64_t []> data;
};
class logging
{
public:
	logging ();
    void serialize_json (boost::property_tree::ptree &) const;
	bool deserialize_json (boost::property_tree::ptree const &);
    bool ledger_logging () const;
    bool ledger_duplicate_logging () const;
    bool network_logging () const;
    bool network_message_logging () const;
    bool network_publish_logging () const;
    bool network_packet_logging () const;
    bool network_keepalive_logging () const;
    bool node_lifetime_tracing () const;
    bool insufficient_work_logging () const;
    bool log_rpc () const;
    bool bulk_pull_logging () const;
    bool work_generation_time () const;
    bool log_to_cerr () const;
	bool ledger_logging_value;
	bool ledger_duplicate_logging_value;
	bool network_logging_value;
	bool network_message_logging_value;
	bool network_publish_logging_value;
	bool network_packet_logging_value;
	bool network_keepalive_logging_value;
	bool node_lifetime_tracing_value;
	bool insufficient_work_logging_value;
	bool log_rpc_value;
	bool bulk_pull_logging_value;
	bool work_generation_time_value;
	bool log_to_cerr_value;
};
class node_init
{
public:
    node_init ();
    bool error ();
    bool block_store_init;
    bool wallet_init;
};
class node_config
{
public:
	node_config ();
	node_config (uint16_t, paper::logging const &);
    void serialize_json (boost::property_tree::ptree &) const;
	bool deserialize_json (boost::property_tree::ptree const &);
	uint16_t peering_port;
	paper::logging logging;
	std::vector <std::string> preconfigured_peers;
	unsigned packet_delay_microseconds;
	unsigned bootstrap_fraction_numerator;
	unsigned creation_rebroadcast;
	unsigned rebroadcast_delay;
    static std::chrono::seconds constexpr keepalive_period = std::chrono::seconds (60);
    static std::chrono::seconds constexpr keepalive_cutoff = keepalive_period * 5;
	static std::chrono::minutes constexpr wallet_backup_interval = std::chrono::minutes (5);
};
class node : public std::enable_shared_from_this <paper::node>
{
public:
    node (paper::node_init &, boost::shared_ptr <boost::asio::io_service>, uint16_t, boost::filesystem::path const &, paper::processor_service &, paper::logging const &, paper::work_pool &);
    node (paper::node_init &, boost::shared_ptr <boost::asio::io_service>, boost::filesystem::path const &, paper::processor_service &, paper::node_config const &, paper::work_pool &);
    ~node ();
	template <typename T>
	void background (T action_a)
	{
		service.add (std::chrono::system_clock::now (), action_a);
	}
    void send_keepalive (paper::endpoint const &);
	void keepalive (std::string const &, uint16_t);
    void start ();
    void stop ();
    std::shared_ptr <paper::node> shared ();
    bool representative_vote (paper::election &, paper::block const &);
    void vote (paper::vote const &);
    void process_confirmed (paper::block const &);
	void process_message (paper::message &, paper::endpoint const &);
    void process_confirmation (paper::block const &, paper::endpoint const &);
    void process_receive_republish (std::unique_ptr <paper::block>, size_t);
    paper::process_return process_receive (paper::block const &);
	paper::process_return process (paper::block const &);
    void keepalive_preconfigured (std::vector <std::string> const &);
	paper::block_hash latest (paper::account const &);
	paper::uint128_t balance (paper::account const &);
	paper::uint128_t weight (paper::account const &);
	paper::account representative (paper::account const &);
	void call_observers (paper::block const & block_a, paper::account const & account_a);
    void ongoing_keepalive ();
	void backup_wallet ();
	int price (paper::uint128_t const &, int);
	paper::node_config config;
    paper::processor_service & service;
	paper::work_pool & work;
    boost::log::sources::logger log;
    paper::block_store store;
    paper::gap_cache gap_cache;
    paper::ledger ledger;
    paper::conflicts conflicts;
    paper::wallets wallets;
    paper::network network;
	paper::bootstrap_initiator bootstrap_initiator;
    paper::bootstrap_listener bootstrap;
    paper::peer_container peers;
	boost::filesystem::path application_path;
    std::vector <std::function <void (paper::block const &, paper::account const &)>> observers;
    std::vector <std::function <void (paper::vote const &)>> vote_observers;
	std::vector <std::function <void (paper::endpoint const &)>> endpoint_observers;
	std::vector <std::function <void ()>> disconnect_observers;
	static double constexpr price_max = 1024.0;
	static double constexpr free_cutoff = 1024.0;
    static std::chrono::seconds constexpr period = std::chrono::seconds (60);
    static std::chrono::seconds constexpr cutoff = period * 5;
	static std::chrono::minutes constexpr backup_interval = std::chrono::minutes (5);
};
class system
{
public:
    system (uint16_t, size_t);
    ~system ();
    void generate_activity (paper::node &);
    void generate_mass_activity (uint32_t, paper::node &);
    void generate_usage_traffic (uint32_t, uint32_t, size_t);
    void generate_usage_traffic (uint32_t, uint32_t);
	paper::account get_random_account (MDB_txn *, paper::node &);
    paper::uint128_t get_random_amount (MDB_txn *, paper::node &, paper::account const &);
    void generate_send_new (paper::node &);
    void generate_send_existing (paper::node &);
    std::shared_ptr <paper::wallet> wallet (size_t);
    paper::account account (MDB_txn *, size_t);
	void poll ();
    boost::shared_ptr <boost::asio::io_service> service;
    paper::processor_service processor;
    std::vector <std::shared_ptr <paper::node>> nodes;
	paper::logging logging;
	paper::work_pool work;
};
class landing_store
{
public:
	landing_store ();
	landing_store (paper::account const &, paper::account const &, uint64_t, uint64_t);
	landing_store (bool &, std::istream &);
	paper::account source;
	paper::account destination;
	uint64_t start;
	uint64_t last;
	bool deserialize (std::istream &);
	void serialize (std::ostream &) const;
	bool operator == (paper::landing_store const &) const;
};
class landing
{
public:
	landing (paper::node &, std::shared_ptr <paper::wallet>, paper::landing_store &, boost::filesystem::path const &);
	void write_store ();
	paper::uint128_t distribution_amount (uint64_t);
	uint64_t seconds_since_epoch ();
	void distribute_one ();
	void distribute_ongoing ();
	boost::filesystem::path path;
	paper::landing_store & store;
	std::shared_ptr <paper::wallet> wallet;
	paper::node & node;
	static int constexpr interval_exponent = 6;
	static std::chrono::seconds constexpr distribution_interval = std::chrono::seconds (1 << interval_exponent); // 64 seconds
	static std::chrono::seconds constexpr sleep_seconds = std::chrono::seconds (7);
};
class thread_runner
{
public:
	thread_runner (boost::asio::io_service &, paper::processor_service &);
	void join ();
	std::vector <std::thread> threads;
};
extern std::chrono::milliseconds const confirm_wait;
}