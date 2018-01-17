#pragma once 

#include <paper/utility.hpp>

#include <boost/property_tree/ptree.hpp>

#include <unordered_map>

namespace std
{
template <>
struct hash <paper::uint256_union>
{
	size_t operator () (paper::uint256_union const & data_a) const
	{
		return *reinterpret_cast <size_t const *> (data_a.bytes.data ());
	}
};
template <>
struct hash <paper::uint256_t>
{
	size_t operator () (paper::uint256_t const & number_a) const
	{
		return number_a.convert_to <size_t> ();
	}
};
}
namespace boost
{
template <>
struct hash <paper::uint256_union>
{
	size_t operator () (paper::uint256_union const & value_a) const
	{
		std::hash <paper::uint256_union> hash;
		return hash (value_a);
	}
};
}
namespace paper
{
class keypair
{
public:
	keypair ();
	keypair (std::string const &);
	paper::public_key pub;
	paper::private_key prv;
};
class block_visitor;
enum class block_type : uint8_t
{
	invalid,
	not_a_block,
	send,
	receive,
	open,
	change
};
class block
{
public:
	// Return a digest of the hashables in this block.
	paper::block_hash hash () const;
	std::string to_json ();
	virtual void hash (blake2b_state &) const = 0;
	virtual uint64_t block_work () const = 0;
	virtual void block_work_set (uint64_t) = 0;
	// Previous block in account's chain, zero for open block
	virtual paper::block_hash previous () const = 0;
	// Source block for open/receive blocks, zero otherwise.
	virtual paper::block_hash source () const = 0;
	// Previous block or account number for open blocks
	virtual paper::block_hash root () const = 0;
	virtual paper::account representative () const = 0;
	virtual void serialize (paper::stream &) const = 0;
	virtual void serialize_json (std::string &) const = 0;
	virtual void visit (paper::block_visitor &) const = 0;
	virtual bool operator == (paper::block const &) const = 0;
	virtual std::unique_ptr <paper::block> clone () const = 0;
	virtual paper::block_type type () const = 0;
	// Local work threshold for rate-limiting publishing blocks. ~5 seconds of work.
	static uint64_t const publish_test_threshold = 0xff00000000000000;
	static uint64_t const publish_full_threshold = 0xfffffe0000000000;
	static uint64_t const publish_threshold = paper::paper_network == paper::paper_networks::paper_test_network ? publish_test_threshold : publish_full_threshold;
};
class unique_ptr_block_hash
{
public:
	size_t operator () (std::unique_ptr <paper::block> const &) const;
	bool operator () (std::unique_ptr <paper::block> const &, std::unique_ptr <paper::block> const &) const;
};
std::unique_ptr <paper::block> deserialize_block (MDB_val const &);
std::unique_ptr <paper::block> deserialize_block (paper::stream &);
std::unique_ptr <paper::block> deserialize_block (paper::stream &, paper::block_type);
std::unique_ptr <paper::block> deserialize_block_json (boost::property_tree::ptree const &);
void serialize_block (paper::stream &, paper::block const &);
bool work_validate (paper::block &);
bool work_validate (paper::block_hash const &, uint64_t);
class send_hashables
{
public:
	send_hashables (paper::account const &, paper::block_hash const &, paper::amount const &);
	send_hashables (bool &, paper::stream &);
	send_hashables (bool &, boost::property_tree::ptree const &);
	void hash (blake2b_state &) const;
	paper::block_hash previous;
	paper::account destination;
	paper::amount balance;
};
class send_block : public paper::block
{
public:
	send_block (paper::block_hash const &, paper::account const &, paper::amount const &, paper::private_key const &, paper::public_key const &, uint64_t);
	send_block (bool &, paper::stream &);
	send_block (bool &, boost::property_tree::ptree const &);
	using paper::block::hash;
	void hash (blake2b_state &) const override;
	uint64_t block_work () const override;
	void block_work_set (uint64_t) override;
	paper::block_hash previous () const override;
	paper::block_hash source () const override;
	paper::block_hash root () const override;
	paper::account representative () const override;
	void serialize (paper::stream &) const override;
	void serialize_json (std::string &) const override;
	bool deserialize (paper::stream &);
	bool deserialize_json (boost::property_tree::ptree const &);
	void visit (paper::block_visitor &) const override;
	std::unique_ptr <paper::block> clone () const override;
	paper::block_type type () const override;
	bool operator == (paper::block const &) const override;
	bool operator == (paper::send_block const &) const;
	static size_t constexpr size = sizeof (paper::account) + sizeof (paper::block_hash) + sizeof (paper::amount) + sizeof (paper::signature) + sizeof (uint64_t);
	send_hashables hashables;
	paper::signature signature;
	uint64_t work;
};
class receive_hashables
{
public:
	receive_hashables (paper::block_hash const &, paper::block_hash const &);
	receive_hashables (bool &, paper::stream &);
	receive_hashables (bool &, boost::property_tree::ptree const &);
	void hash (blake2b_state &) const;
	paper::block_hash previous;
	paper::block_hash source;
};
class receive_block : public paper::block
{
public:
	receive_block (paper::block_hash const &, paper::block_hash const &, paper::private_key const &, paper::public_key const &, uint64_t);
	receive_block (bool &, paper::stream &);
	receive_block (bool &, boost::property_tree::ptree const &);
	using paper::block::hash;
	void hash (blake2b_state &) const override;
	uint64_t block_work () const override;
	void block_work_set (uint64_t) override;
	paper::block_hash previous () const override;
	paper::block_hash source () const override;
	paper::block_hash root () const override;
	paper::account representative () const override;
	void serialize (paper::stream &) const override;
	void serialize_json (std::string &) const override;
	bool deserialize (paper::stream &);
	bool deserialize_json (boost::property_tree::ptree const &);
	void visit (paper::block_visitor &) const override;
	std::unique_ptr <paper::block> clone () const override;
	paper::block_type type () const override;
	bool operator == (paper::block const &) const override;
	bool operator == (paper::receive_block const &) const;
	static size_t constexpr size = sizeof (paper::block_hash) + sizeof (paper::block_hash) + sizeof (paper::signature) + sizeof (uint64_t);
	receive_hashables hashables;
	paper::signature signature;
	uint64_t work;
};
class open_hashables
{
public:
	open_hashables (paper::block_hash const &, paper::account const &, paper::account const &);
	open_hashables (bool &, paper::stream &);
	open_hashables (bool &, boost::property_tree::ptree const &);
	void hash (blake2b_state &) const;
	paper::block_hash source;
	paper::account representative;
	paper::account account;
};
class open_block : public paper::block
{
public:
	open_block (paper::block_hash const &, paper::account const &, paper::account const &, paper::private_key const &, paper::public_key const &, uint64_t);
	open_block (paper::block_hash const &, paper::account const &, paper::account const &, std::nullptr_t);
	open_block (bool &, paper::stream &);
	open_block (bool &, boost::property_tree::ptree const &);
	using paper::block::hash;
	void hash (blake2b_state &) const override;
	uint64_t block_work () const override;
	void block_work_set (uint64_t) override;
	paper::block_hash previous () const override;
	paper::block_hash source () const override;
	paper::block_hash root () const override;
	paper::account representative () const override;
	void serialize (paper::stream &) const override;
	void serialize_json (std::string &) const override;
	bool deserialize (paper::stream &);
	bool deserialize_json (boost::property_tree::ptree const &);
	void visit (paper::block_visitor &) const override;
	std::unique_ptr <paper::block> clone () const override;
	paper::block_type type () const override;
	bool operator == (paper::block const &) const override;
	bool operator == (paper::open_block const &) const;
	static size_t constexpr size = sizeof (paper::block_hash) + sizeof (paper::account) + sizeof (paper::account) + sizeof (paper::signature) + sizeof (uint64_t);
	paper::open_hashables hashables;
	paper::signature signature;
	uint64_t work;
};
class change_hashables
{
public:
	change_hashables (paper::block_hash const &, paper::account const &);
	change_hashables (bool &, paper::stream &);
	change_hashables (bool &, boost::property_tree::ptree const &);
	void hash (blake2b_state &) const;
	paper::block_hash previous;
	paper::account representative;
};
class change_block : public paper::block
{
public:
	change_block (paper::block_hash const &, paper::account const &, paper::private_key const &, paper::public_key const &, uint64_t);
	change_block (bool &, paper::stream &);
	change_block (bool &, boost::property_tree::ptree const &);
	using paper::block::hash;
	void hash (blake2b_state &) const override;
	uint64_t block_work () const override;
	void block_work_set (uint64_t) override;
	paper::block_hash previous () const override;
	paper::block_hash source () const override;
	paper::block_hash root () const override;
	paper::account representative () const override;
	void serialize (paper::stream &) const override;
	void serialize_json (std::string &) const override;
	bool deserialize (paper::stream &);
	bool deserialize_json (boost::property_tree::ptree const &);
	void visit (paper::block_visitor &) const override;
	std::unique_ptr <paper::block> clone () const override;
	paper::block_type type () const override;
	bool operator == (paper::block const &) const override;
	bool operator == (paper::change_block const &) const;
	static size_t constexpr size = sizeof (paper::block_hash) + sizeof (paper::account) + sizeof (paper::signature) + sizeof (uint64_t);
	paper::change_hashables hashables;
	paper::signature signature;
	uint64_t work;
};
class block_visitor
{
public:
	virtual void send_block (paper::send_block const &) = 0;
	virtual void receive_block (paper::receive_block const &) = 0;
	virtual void open_block (paper::open_block const &) = 0;
	virtual void change_block (paper::change_block const &) = 0;
};
// Latest information about an account
class account_info
{
public:
	account_info ();
	account_info (MDB_val const &);
	account_info (paper::account_info const &) = default;
	account_info (paper::block_hash const &, paper::block_hash const &, paper::amount const &, uint64_t, bool);
	void serialize (paper::stream &) const;
	bool deserialize (paper::stream &);
	bool operator == (paper::account_info const &) const;
	bool operator != (paper::account_info const &) const;
	paper::mdb_val val () const;
	paper::block_hash head;
	paper::block_hash rep_block;
	paper::amount balance;
	uint64_t modified;
};
class store_entry
{
public:
	store_entry ();
	void clear ();
	store_entry * operator -> ();
	MDB_val first;
	MDB_val second;
};
class store_iterator
{
public:
	store_iterator (MDB_txn *, MDB_dbi);
	store_iterator (std::nullptr_t);
	store_iterator (MDB_txn *, MDB_dbi, MDB_val const &);
	store_iterator (paper::store_iterator &&);
	store_iterator (paper::store_iterator const &) = delete;
	~store_iterator ();
	paper::store_iterator & operator ++ ();
	paper::store_iterator & operator = (paper::store_iterator &&);
	paper::store_iterator & operator = (paper::store_iterator const &) = delete;
	paper::store_entry & operator -> ();
	bool operator == (paper::store_iterator const &) const;
	bool operator != (paper::store_iterator const &) const;
	MDB_cursor * cursor;
	paper::store_entry current;
};
// Information on an uncollected send, source account, amount, target account.
class receivable
{
public:
	receivable ();
	receivable (MDB_val const &);
	receivable (paper::account const &, paper::amount const &, paper::account const &);
	void serialize (paper::stream &) const;
	bool deserialize (paper::stream &);
	bool operator == (paper::receivable const &) const;
	paper::mdb_val val () const;
	paper::account source;
	paper::amount amount;
	paper::account destination;
};
class block_store
{
public:
	block_store (bool &, boost::filesystem::path const &);
	uint64_t now ();
	
	MDB_dbi block_database (paper::block_type);
	void block_put_raw (MDB_txn *, MDB_dbi, paper::block_hash const &, MDB_val);
	void block_put (MDB_txn *, paper::block_hash const &, paper::block const &);
	MDB_val block_get_raw (MDB_txn *, paper::block_hash const &, paper::block_type &);
	paper::block_hash block_successor (MDB_txn *, paper::block_hash const &);
	std::unique_ptr <paper::block> block_get (MDB_txn *, paper::block_hash const &);
	void block_del (MDB_txn *, paper::block_hash const &);
	bool block_exists (MDB_txn *, paper::block_hash const &);
	
	void frontier_put (MDB_txn *, paper::block_hash const &, paper::account const &);
	paper::account frontier_get (MDB_txn *, paper::block_hash const &);
	void frontier_del (MDB_txn *, paper::block_hash const &);
	
	void account_put (MDB_txn *, paper::account const &, paper::account_info const &);
	bool account_get (MDB_txn *, paper::account const &, paper::account_info &);
	void account_del (MDB_txn *, paper::account const &);
	bool account_exists (paper::account const &);
	paper::store_iterator latest_begin (MDB_txn *, paper::account const &);
	paper::store_iterator latest_begin (MDB_txn *);
	paper::store_iterator latest_end ();
	
	void pending_put (MDB_txn *, paper::block_hash const &, paper::receivable const &);
	void pending_del (MDB_txn *, paper::block_hash const &);
	bool pending_get (MDB_txn *, paper::block_hash const &, paper::receivable &);
	bool pending_exists (MDB_txn *, paper::block_hash const &);
	paper::store_iterator pending_begin (MDB_txn *, paper::block_hash const &);
	paper::store_iterator pending_begin (MDB_txn *);
	paper::store_iterator pending_end ();
	
	paper::uint128_t representation_get (MDB_txn *, paper::account const &);
	void representation_put (MDB_txn *, paper::account const &, paper::uint128_t const &);
	
	void unchecked_put (MDB_txn *, paper::block_hash const &, paper::block const &);
	std::unique_ptr <paper::block> unchecked_get (MDB_txn *, paper::block_hash const &);
	void unchecked_del (MDB_txn *, paper::block_hash const &);
	paper::store_iterator unchecked_begin (MDB_txn *);
	paper::store_iterator unchecked_end ();
	
	void unsynced_put (MDB_txn *, paper::block_hash const &);
	void unsynced_del (MDB_txn *, paper::block_hash const &);
	bool unsynced_exists (MDB_txn *, paper::block_hash const &);
	paper::store_iterator unsynced_begin (MDB_txn *, paper::block_hash const &);
	paper::store_iterator unsynced_begin (MDB_txn *);
	paper::store_iterator unsynced_end ();

	void stack_open ();
	void stack_push (uint64_t, paper::block_hash const &);
	paper::block_hash stack_pop (uint64_t);
	
	void checksum_put (MDB_txn *, uint64_t, uint8_t, paper::checksum const &);
	bool checksum_get (MDB_txn *, uint64_t, uint8_t, paper::checksum &);
	void checksum_del (MDB_txn *, uint64_t, uint8_t);
	
	void clear (MDB_dbi);
	
	paper::mdb_env environment;
	// block_hash -> account                                        // Maps head blocks to owning account
	MDB_dbi frontiers;
	// account -> block_hash, representative, balance, timestamp    // Account to head block, representative, balance, last_change
	MDB_dbi accounts;
	// block_hash -> send_block
	MDB_dbi send_blocks;
	// block_hash -> receive_block
	MDB_dbi receive_blocks;
	// block_hash -> open_block
	MDB_dbi open_blocks;
	// block_hash -> change_block
	MDB_dbi change_blocks;
	// block_hash -> sender, amount, destination                    // Pending blocks to sender account, amount, destination account
	MDB_dbi pending;
	// account -> weight                                            // Representation
	MDB_dbi representation;
	// block_hash -> block                                          // Unchecked bootstrap blocks
	MDB_dbi unchecked;
	// block_hash ->                                                // Blocks that haven't been broadcast
	MDB_dbi unsynced;
	// uint64_t -> block_hash                                       // Block dependency stack while bootstrapping
	MDB_dbi stack;
	// (uint56_t, uint8_t) -> block_hash                            // Mapping of region to checksum
	MDB_dbi checksum;
};
enum class process_result
{
	progress, // Hasn't been seen before, signed correctly
	bad_signature, // Signature was bad, forged or transmission error
	old, // Already seen and was valid
	overspend, // Malicious attempt to overspend
	fork, // Malicious fork based on previous
	unreceivable, // Source block doesn't exist or has already been received
	gap_previous, // Block marked as previous is unknown
	gap_source, // Block marked as source is unknown
	not_receive_from_send, // Receive does not have a send source
	account_mismatch // Account number in open block doesn't match send destination
};
class process_return
{
public:
	paper::process_result code;
	paper::account account;
};
class vote
{
public:
	vote () = default;
	vote (bool &, paper::stream &, paper::block_type);
	vote (paper::account const &, paper::private_key const &, uint64_t, std::unique_ptr <paper::block>);
	paper::uint256_union hash () const;
	// Vote round sequence number
	uint64_t sequence;
	std::unique_ptr <paper::block> block;
	// Account that's voting
	paper::account account;
	// Signature of sequence + block hash
	paper::signature signature;
};
class votes
{
public:
	votes (paper::block_hash const &);
	bool vote (paper::vote const &);
	// Our vote round sequence number
	uint64_t sequence;
	// Root block of fork
	paper::block_hash id;
	// All votes received by account
	std::unordered_map <paper::account, std::pair <uint64_t, std::unique_ptr <paper::block>>> rep_votes;
};
class ledger
{
public:
	ledger (paper::block_store &);
	std::pair <paper::uint128_t, std::unique_ptr <paper::block>> winner (MDB_txn *, paper::votes const & votes_a);
	std::map <paper::uint128_t, std::unique_ptr <paper::block>, std::greater <paper::uint128_t>> tally (MDB_txn *, paper::votes const &);
	paper::account account (MDB_txn *, paper::block_hash const &);
	paper::uint128_t amount (MDB_txn *, paper::block_hash const &);
	paper::uint128_t balance (MDB_txn *, paper::block_hash const &);
	paper::uint128_t account_balance (MDB_txn *, paper::account const &);
	paper::uint128_t weight (MDB_txn *, paper::account const &);
	std::unique_ptr <paper::block> successor (MDB_txn *, paper::block_hash const &);
	paper::block_hash latest (MDB_txn *, paper::account const &);
	paper::block_hash latest_root (MDB_txn *, paper::account const &);
	paper::account representative (MDB_txn *, paper::block_hash const &);
	paper::account representative_calculated (MDB_txn *, paper::block_hash const &);
	paper::uint128_t supply (MDB_txn *);
	paper::process_return process (MDB_txn *, paper::block const &);
	void rollback (MDB_txn *, paper::block_hash const &);
	void change_latest (MDB_txn *, paper::account const &, paper::block_hash const &, paper::account const &, paper::uint128_union const &);
	void move_representation (MDB_txn *, paper::account const &, paper::account const &, paper::uint128_t const &);
	void checksum_update (MDB_txn *, paper::block_hash const &);
	paper::checksum checksum (MDB_txn *, paper::account const &, paper::account const &);
	void dump_account_chain (paper::account const &);
	static paper::uint128_t const unit;
	paper::block_store & store;
};
extern paper::keypair const zero_key;
extern paper::keypair const test_genesis_key;
extern paper::account const paper_test_account;
extern paper::account const paper_beta_account;
extern paper::account const paper_live_account;
extern paper::account const genesis_account;
extern paper::uint128_t const genesis_amount;
class genesis
{
public:
	explicit genesis ();
	void initialize (MDB_txn *, paper::block_store &) const;
	paper::block_hash hash () const;
	paper::open_block open;
};
}