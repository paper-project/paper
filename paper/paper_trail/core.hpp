#pragma once

#include <paper/secure.hpp>

#include <boost/asio.hpp>
#include <boost/network/include/http/server.hpp>
#include <boost/network/utils/thread_pool.hpp>
#include <boost/iostreams/device/array.hpp>
#include <boost/multi_index_container.hpp>
#include <boost/multi_index/ordered_index.hpp>
#include <boost/multi_index/hashed_index.hpp>
#include <boost/multi_index/member.hpp>
#include <boost/multi_index/random_access_index.hpp>
#include <boost/circular_buffer.hpp>

#include <unordered_set>
#include <memory>
#include <queue>
#include <mutex>
#include <condition_variable>

namespace CryptoPP
{
    class SHA3;
}

std::ostream & operator << (std::ostream &, std::chrono::system_clock::time_point const &);
namespace paper {
    using endpoint = boost::asio::ip::udp::endpoint;
    using tcp_endpoint = boost::asio::ip::tcp::endpoint;
    bool parse_endpoint (std::string const &, paper::endpoint &);
    bool parse_tcp_endpoint (std::string const &, paper::tcp_endpoint &);
	bool reserved_address (paper::endpoint const &);
}

namespace std
{
    template <size_t size>
    struct endpoint_hash
    {
    };
    template <>
    struct endpoint_hash <4>
    {
        size_t operator () (paper::endpoint const & endpoint_a) const
        {
            assert (endpoint_a.address ().is_v6 ());
            paper::uint128_union address;
            address.bytes = endpoint_a.address ().to_v6 ().to_bytes ();
            auto result (address.dwords [0] ^ address.dwords [1] ^ address.dwords [2] ^ address.dwords [3] ^ endpoint_a.port ());
            return result;
        }
    };
    template <>
    struct endpoint_hash <8>
    {
        size_t operator () (paper::endpoint const & endpoint_a) const
        {
            assert (endpoint_a.address ().is_v6 ());
            paper::uint128_union address;
            address.bytes = endpoint_a.address ().to_v6 ().to_bytes ();
            auto result (address.qwords [0] ^ address.qwords [1] ^ endpoint_a.port ());
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

namespace paper {
    class client;
    class destructable
    {
    public:
        destructable (std::function <void ()>);
        ~destructable ();
        std::function <void ()> operation;
    };
	class election : public std::enable_shared_from_this <paper::election>
	{
	public:
        election (std::shared_ptr <paper::client>, paper::block const &, paper::uint256_union const &);
        void start ();
        void vote (paper::vote const &);
        void announce_vote ();
        void timeout_action (std::shared_ptr <paper::destructable>);
        void start_request (paper::block const &);
		paper::uint256_t uncontested_threshold ();
		paper::uint256_t contested_threshold ();
		paper::votes votes;
        std::shared_ptr <paper::client> client;
		std::chrono::system_clock::time_point last_vote;
		bool confirmed;
        paper::uint256_union work;
	};
    class conflicts
    {
    public:
		conflicts (paper::client &);
        void start (paper::block const &, paper::uint256_union const &, bool);
		void update (paper::vote const &);
        void stop (paper::block_hash const &);
        std::unordered_map <paper::block_hash, std::shared_ptr <paper::election>> roots;
		paper::client & client;
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
        confirm_unk,
        bulk_req,
		frontier_req
    };
    class message_visitor;
    class message
    {
    public:
        message (paper::message_type);
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
        static std::array <uint8_t, 2> constexpr magic_number = {{'R', 'A'}};
        uint8_t version_max;
        uint8_t version_using;
        uint8_t version_min;
        paper::message_type type;
        std::bitset <16> extensions;
        static size_t constexpr test_network_position = 0;
        static size_t constexpr ipv4_only_position = 1;
        static size_t constexpr bootstrap_receiver_position = 2;
        static std::bitset <16> constexpr block_type_mask = std::bitset <16> (0x0f00);
    };
    class keepalive : public message
    {
    public:
        keepalive ();
        void visit (paper::message_visitor &) const override;
        bool deserialize (paper::stream &);
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
        bool deserialize (paper::stream &);
        void serialize (paper::stream &) override;
        bool operator == (paper::publish const &) const;
        paper::uint256_union work;
        std::unique_ptr <paper::block> block;
    };
    class confirm_req : public message
    {
    public:
        confirm_req ();
        confirm_req (std::unique_ptr <paper::block>);
        bool deserialize (paper::stream &);
        void serialize (paper::stream &) override;
        void visit (paper::message_visitor &) const override;
        bool operator == (paper::confirm_req const &) const;
        paper::uint256_union work;
        std::unique_ptr <paper::block> block;
    };
    class confirm_ack : public message
    {
    public:
        confirm_ack ();
        confirm_ack (std::unique_ptr <paper::block>);
        bool deserialize (paper::stream &);
        void serialize (paper::stream &) override;
        void visit (paper::message_visitor &) const override;
        bool operator == (paper::confirm_ack const &) const;
        paper::vote vote;
        paper::uint256_union work;
    };
    class confirm_unk : public message
    {
    public:
        confirm_unk ();
        bool deserialize (paper::stream &);
        void serialize (paper::stream &) override;
        void visit (paper::message_visitor &) const override;
		paper::uint256_union hash () const;
        paper::address rep_hint;
    };
    class frontier_req : public message
    {
    public:
        frontier_req ();
        bool deserialize (paper::stream &);
        void serialize (paper::stream &) override;
        void visit (paper::message_visitor &) const override;
        bool operator == (paper::frontier_req const &) const;
        paper::address start;
        uint32_t age;
        uint32_t count;
    };
    class bulk_req : public message
    {
    public:
        bulk_req ();
        bool deserialize (paper::stream &);
        void serialize (paper::stream &) override;
        void visit (paper::message_visitor &) const override;
        paper::uint256_union start;
        paper::block_hash end;
        uint32_t count;
    };
    class message_visitor
    {
    public:
        virtual void keepalive (paper::keepalive const &) = 0;
        virtual void publish (paper::publish const &) = 0;
        virtual void confirm_req (paper::confirm_req const &) = 0;
        virtual void confirm_ack (paper::confirm_ack const &) = 0;
        virtual void confirm_unk (paper::confirm_unk const &) = 0;
        virtual void bulk_req (paper::bulk_req const &) = 0;
        virtual void frontier_req (paper::frontier_req const &) = 0;
    };
    class key_entry
    {
    public:
        paper::key_entry * operator -> ();
        paper::public_key first;
        paper::private_key second;
    };
    class key_iterator
    {
    public:
        key_iterator (leveldb::DB *); // Begin iterator
        key_iterator (leveldb::DB *, std::nullptr_t); // End iterator
        key_iterator (leveldb::DB *, paper::uint256_union const &);
        key_iterator (paper::key_iterator &&) = default;
        void set_current ();
        key_iterator & operator ++ ();
        paper::key_entry & operator -> ();
        bool operator == (paper::key_iterator const &) const;
        bool operator != (paper::key_iterator const &) const;
        paper::key_entry current;
        std::unique_ptr <leveldb::Iterator> iterator;
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
    class wallet
    {
    public:
        wallet (bool &, boost::filesystem::path const &);
        paper::uint256_union check ();
        bool rekey (std::string const &);
        paper::uint256_union wallet_key ();
        paper::uint256_union salt ();
        void insert (paper::private_key const &);
        bool fetch (paper::public_key const &, paper::private_key &);
        bool generate_send (paper::ledger &, paper::public_key const &, paper::uint128_t const &, std::vector <std::unique_ptr <paper::send_block>> &);
		bool valid_password ();
        key_iterator find (paper::uint256_union const &);
        key_iterator begin ();
        key_iterator end ();
        paper::uint256_union derive_key (std::string const &);
        paper::fan password;
        static paper::uint256_union const version_1;
        static paper::uint256_union const version_current;
        static paper::uint256_union const version_special;
        static paper::uint256_union const wallet_key_special;
        static paper::uint256_union const salt_special;
        static paper::uint256_union const check_special;
        static int const special_count;
    private:
        std::unique_ptr <leveldb::DB> handle;
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
    class peer_information
    {
    public:
        paper::endpoint endpoint;
        std::chrono::system_clock::time_point last_contact;
        std::chrono::system_clock::time_point last_attempt;
    };
    class gap_information
    {
    public:
        std::chrono::system_clock::time_point arrival;
        paper::block_hash hash;
        std::unique_ptr <paper::block> block;
    };
    class gap_cache
    {
    public:
        gap_cache ();
        void add (paper::block const &, paper::block_hash);
        std::unique_ptr <paper::block> get (paper::block_hash const &);
        boost::multi_index_container
        <
            gap_information,
            boost::multi_index::indexed_by
            <
                boost::multi_index::hashed_unique <boost::multi_index::member <gap_information, paper::block_hash, &gap_information::hash>>,
                boost::multi_index::ordered_non_unique <boost::multi_index::member <gap_information, std::chrono::system_clock::time_point, &gap_information::arrival>>
            >
        > blocks;
        size_t const max;
    };
    using session = std::function <void (paper::confirm_ack const &, paper::endpoint const &)>;
    class processor
    {
    public:
        processor (paper::client &);
        void stop ();
        void find_network (std::vector <std::pair <std::string, std::string>> const &);
        void bootstrap (paper::tcp_endpoint const &, std::function <void ()> const &);
        void connect_bootstrap (std::vector <std::string> const &);
        paper::process_result process_receive (paper::block const &, std::function <paper::uint256_union (paper::block const &)>);
        void process_receive_republish (std::unique_ptr <paper::block>, std::function <paper::uint256_union (paper::block const &)>, paper::endpoint const &);
        void republish (std::unique_ptr <paper::block>, paper::uint256_union const &, paper::endpoint const &);
		void process_message (paper::message &, paper::endpoint const &);
		void process_unknown (paper::vectorstream &);
        void process_confirmation (paper::block const &, paper::uint256_union const &, paper::endpoint const &);
        void process_confirmed (paper::block const &);
        void ongoing_keepalive ();
        paper::client & client;
        static std::chrono::seconds constexpr period = std::chrono::seconds (60);
        static std::chrono::seconds constexpr cutoff = period * 5;
    };
    class transactions
    {
    public:
        transactions (paper::client &);
        bool receive (paper::send_block const &, paper::private_key const &, paper::address const &);
        bool send (paper::address const &, paper::uint128_t const &);
        void vote (paper::vote const &);
        bool rekey (std::string const &);
        std::mutex mutex;
        paper::client & client;
    };
    class bootstrap_initiator : public std::enable_shared_from_this <bootstrap_initiator>
    {
    public:
        bootstrap_initiator (std::shared_ptr <paper::client>, std::function <void ()> const &);
        ~bootstrap_initiator ();
        void run (paper::tcp_endpoint const &);
        void connect_action (boost::system::error_code const &);
        void send_frontier_request ();
        void sent_request (boost::system::error_code const &, size_t);
        void run_receiver ();
        void finish_request ();
        void add_and_send (std::unique_ptr <paper::message>);
        void add_request (std::unique_ptr <paper::message>);
        std::queue <std::unique_ptr <paper::message>> requests;
        std::vector <uint8_t> send_buffer;
        std::shared_ptr <paper::client> client;
        boost::asio::ip::tcp::socket socket;
        std::function <void ()> complete_action;
        std::mutex mutex;
        static size_t const max_queue_size = 10;
    };
    class bulk_req_initiator : public std::enable_shared_from_this <bulk_req_initiator>
    {
    public:
        bulk_req_initiator (std::shared_ptr <paper::bootstrap_initiator> const &, std::unique_ptr <paper::bulk_req>);
        ~bulk_req_initiator ();
        void receive_block ();
        void received_type (boost::system::error_code const &, size_t);
        void received_block (boost::system::error_code const &, size_t);
        bool process_block (paper::block const &);
        bool process_end ();
        std::array <uint8_t, 4000> receive_buffer;
        std::unique_ptr <paper::bulk_req> request;
        paper::block_hash expecting;
        std::shared_ptr <paper::bootstrap_initiator> connection;
    };
    class frontier_req_initiator : public std::enable_shared_from_this <frontier_req_initiator>
    {
    public:
        frontier_req_initiator (std::shared_ptr <paper::bootstrap_initiator> const &, std::unique_ptr <paper::frontier_req>);
        ~frontier_req_initiator ();
        void receive_frontier ();
        void received_frontier (boost::system::error_code const &, size_t);
        std::array <uint8_t, 4000> receive_buffer;
        std::unique_ptr <paper::frontier_req> request;
        std::shared_ptr <paper::bootstrap_initiator> connection;
    };
    class work
    {
    public:
        work ();
        paper::uint256_union generate (paper::uint256_union const &, paper::uint256_union const &);
        paper::uint256_union create (paper::uint256_union const &);
        bool validate (paper::uint256_union const &, paper::uint256_union const &);
        paper::uint256_union threshold_requirement;
        size_t const entry_requirement;
        uint32_t const iteration_requirement;
        std::vector <uint64_t> entries;
    };
    class network
    {
    public:
        network (boost::asio::io_service &, uint16_t, paper::client &);
        void receive ();
        void stop ();
        void receive_action (boost::system::error_code const &, size_t);
        void rpc_action (boost::system::error_code const &, size_t);
        void publish_block (paper::endpoint const &, std::unique_ptr <paper::block>, paper::uint256_union const &);
        void confirm_block (std::unique_ptr <paper::block>, paper::uint256_union const &, uint64_t);
        void merge_peers (std::array <paper::endpoint, 8> const &);
        void refresh_keepalive (paper::endpoint const &);
        void send_keepalive (paper::endpoint const &);
        void send_confirm_req (paper::endpoint const &, paper::block const &, paper::uint256_union const &);
        void send_buffer (uint8_t const *, size_t, paper::endpoint const &, std::function <void (boost::system::error_code const &, size_t)>);
        void send_complete (boost::system::error_code const &, size_t);
        paper::endpoint endpoint ();
        paper::endpoint remote;
        std::array <uint8_t, 512> buffer;
        paper::work work;
        std::mutex work_mutex;
        boost::asio::ip::udp::socket socket;
        std::mutex socket_mutex;
        boost::asio::io_service & service;
        boost::asio::ip::udp::resolver resolver;
        paper::client & client;
        std::queue <std::tuple <uint8_t const *, size_t, paper::endpoint, std::function <void (boost::system::error_code const &, size_t)>>> sends;
        uint64_t keepalive_count;
        uint64_t publish_req_count;
        uint64_t confirm_req_count;
        uint64_t confirm_ack_count;
        uint64_t confirm_unk_count;
        uint64_t bad_sender_count;
        uint64_t unknown_count;
        uint64_t error_count;
        uint64_t insufficient_work_count;
        bool on;
    };
    class bootstrap_receiver
    {
    public:
        bootstrap_receiver (boost::asio::io_service &, uint16_t, paper::client &);
        void start ();
        void stop ();
        void accept_connection ();
        void accept_action (boost::system::error_code const &, std::shared_ptr <boost::asio::ip::tcp::socket>);
        paper::tcp_endpoint endpoint ();
        boost::asio::ip::tcp::acceptor acceptor;
        paper::tcp_endpoint local;
        boost::asio::io_service & service;
        paper::client & client;
        bool on;
    };
    class bootstrap_connection : public std::enable_shared_from_this <bootstrap_connection>
    {
    public:
        bootstrap_connection (std::shared_ptr <boost::asio::ip::tcp::socket>, std::shared_ptr <paper::client>);
        ~bootstrap_connection ();
        void receive ();
        void receive_header_action (boost::system::error_code const &, size_t);
        void receive_bulk_req_action (boost::system::error_code const &, size_t);
		void receive_frontier_req_action (boost::system::error_code const &, size_t);
		void add_request (std::unique_ptr <paper::message>);
		void finish_request ();
		void run_next ();
        std::array <uint8_t, 128> receive_buffer;
        std::shared_ptr <boost::asio::ip::tcp::socket> socket;
        std::shared_ptr <paper::client> client;
        std::mutex mutex;
        std::queue <std::unique_ptr <paper::message>> requests;
    };
    class bulk_req_response : public std::enable_shared_from_this <bulk_req_response>
    {
    public:
        bulk_req_response (std::shared_ptr <paper::bootstrap_connection> const &, std::unique_ptr <paper::bulk_req>);
        void set_current_end ();
        std::unique_ptr <paper::block> get_next ();
        void send_next ();
        void sent_action (boost::system::error_code const &, size_t);
        void send_finished ();
        void no_block_sent (boost::system::error_code const &, size_t);
        std::shared_ptr <paper::bootstrap_connection> connection;
        std::unique_ptr <paper::bulk_req> request;
        std::vector <uint8_t> send_buffer;
        paper::block_hash current;
    };
    class frontier_req_response : public std::enable_shared_from_this <frontier_req_response>
    {
    public:
        frontier_req_response (std::shared_ptr <paper::bootstrap_connection> const &, std::unique_ptr <paper::frontier_req>);
        void skip_old ();
		void send_next ();
        void sent_action (boost::system::error_code const &, size_t);
        void send_finished ();
        void no_block_sent (boost::system::error_code const &, size_t);
        std::pair <paper::uint256_union, paper::uint256_union> get_next ();
        std::shared_ptr <paper::bootstrap_connection> connection;
		account_iterator iterator;
        std::unique_ptr <paper::frontier_req> request;
        std::vector <uint8_t> send_buffer;
        size_t count;
    };
    class rpc
    {
    public:
        rpc (boost::shared_ptr <boost::asio::io_service>, boost::shared_ptr <boost::network::utils::thread_pool>, boost::asio::ip::address_v6 const &, uint16_t, paper::client &, bool);
        void start ();
        void stop ();
        boost::network::http::server <paper::rpc> server;
        void operator () (boost::network::http::server <paper::rpc>::request const &, boost::network::http::server <paper::rpc>::response &);
        void log (const char *) {}
        paper::client & client;
        bool on;
        bool enable_control;
    };
    class peer_container
    {
    public:
		peer_container (paper::endpoint const &);
        bool known_peer (paper::endpoint const &);
        void incoming_from_peer (paper::endpoint const &);
        // Returns true if peer was already known
		bool insert_peer (paper::endpoint const &);
		void random_fill (std::array <paper::endpoint, 8> &);
        std::vector <peer_information> list ();
        void refresh_action ();
        void queue_next_refresh ();
        std::vector <paper::peer_information> purge_list (std::chrono::system_clock::time_point const &);
        size_t size ();
        bool empty ();
        std::mutex mutex;
		paper::endpoint self;
        boost::multi_index_container
        <peer_information,
            boost::multi_index::indexed_by
            <
                boost::multi_index::hashed_unique <boost::multi_index::member <peer_information, paper::endpoint, &peer_information::endpoint>>,
                boost::multi_index::ordered_non_unique <boost::multi_index::member <peer_information, std::chrono::system_clock::time_point, &peer_information::last_contact>>,
                boost::multi_index::ordered_non_unique <boost::multi_index::member <peer_information, std::chrono::system_clock::time_point, &peer_information::last_attempt>, std::greater <std::chrono::system_clock::time_point>>
            >
        > peers;
    };
    class log
    {
    public:
        log ();
        void add (std::string const &);
        void dump_cerr ();
        boost::circular_buffer <std::pair <std::chrono::system_clock::time_point, std::string>> items;
    };
    class client_init
    {
    public:
        client_init ();
        bool error ();
        leveldb::Status block_store_init;
        bool wallet_init;
        bool ledger_init;
    };
    class client : public std::enable_shared_from_this <paper::client>
    {
    public:
        client (paper::client_init &, boost::shared_ptr <boost::asio::io_service>, uint16_t, boost::filesystem::path const &, paper::processor_service &, paper::address const &);
        client (paper::client_init &, boost::shared_ptr <boost::asio::io_service>, uint16_t, paper::processor_service &, paper::address const &);
        ~client ();
        bool send (paper::public_key const &, paper::uint128_t const &);
        paper::uint256_t balance ();
        void start ();
        void stop ();
        std::shared_ptr <paper::client> shared ();
        bool is_representative ();
		void representative_vote (paper::election &, paper::block const &);
        paper::uint256_union create_work (paper::block const &);
        paper::log log;
        paper::address representative;
        paper::block_store store;
        paper::gap_cache gap_cache;
        paper::ledger ledger;
        paper::conflicts conflicts;
        paper::wallet wallet;
        paper::network network;
        paper::bootstrap_receiver bootstrap;
        paper::processor processor;
        paper::transactions transactions;
        paper::peer_container peers;
        paper::processor_service & service;
    };
    class system
    {
    public:
        system (uint16_t, size_t);
        ~system ();
        void generate_activity (paper::client &);
        void generate_mass_activity (uint32_t, paper::client &);
        void generate_usage_traffic (uint32_t, uint32_t, size_t);
        void generate_usage_traffic (uint32_t, uint32_t);
        paper::uint128_t get_random_amount (paper::client &);
        void generate_send_new (paper::client &);
        void generate_send_existing (paper::client &);
        boost::shared_ptr <boost::asio::io_service> service;
        paper::processor_service processor;
        std::vector <std::shared_ptr <paper::client>> clients;
    };
}