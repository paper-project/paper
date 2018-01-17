#pragma once

#include <array>
#include <condition_variable>
#include <type_traits>

#include <blake2/blake2.h>

#include <boost/iostreams/device/back_inserter.hpp>
#include <boost/iostreams/stream.hpp>
#include <boost/filesystem.hpp>
#include <boost/multiprecision/cpp_int.hpp>

#include <cryptopp/osrng.h>

#include <liblmdb/lmdb.h>

#include <paper/config.hpp>

namespace paper
{
extern CryptoPP::AutoSeededRandomPool random_pool;
// We operate on streams of uint8_t by convention
using stream = std::basic_streambuf <uint8_t>;
using bufferstream = boost::iostreams::stream_buffer <boost::iostreams::basic_array_source <uint8_t>>;
using vectorstream = boost::iostreams::stream_buffer <boost::iostreams::back_insert_device <std::vector <uint8_t>>>;
// OS-specific way of finding a path to a home directory.
boost::filesystem::path working_path ();
// Get a unique path within the home directory, used for testing
boost::filesystem::path unique_path ();
// Read a raw byte stream the size of `T' and fill value.
template <typename T>
bool read (paper::stream & stream_a, T & value)
{
	static_assert (std::is_pod <T>::value, "Can't stream read non-standard layout types");
	auto amount_read (stream_a.sgetn (reinterpret_cast <uint8_t *> (&value), sizeof (value)));
	return amount_read != sizeof (value);
}
template <typename T>
void write (paper::stream & stream_a, T const & value)
{
	static_assert (std::is_pod <T>::value, "Can't stream write non-standard layout types");
	auto amount_written (stream_a.sputn (reinterpret_cast <uint8_t const *> (&value), sizeof (value)));
	assert (amount_written == sizeof (value));
}
std::string to_string_hex (uint64_t);
bool from_string_hex (std::string const &, uint64_t &);

using uint128_t = boost::multiprecision::uint128_t;
using uint256_t = boost::multiprecision::uint256_t;
using uint512_t = boost::multiprecision::uint512_t;
// SI dividers
paper::uint128_t const Gpaper_ratio = paper::uint128_t ("1000000000000000000000000000000000"); // 10^33
paper::uint128_t const Mpaper_ratio = paper::uint128_t ("1000000000000000000000000000000"); // 10^30
paper::uint128_t const kpaper_ratio = paper::uint128_t ("1000000000000000000000000000"); // 10^27
paper::uint128_t const  paper_ratio = paper::uint128_t ("1000000000000000000000000"); // 10^24
paper::uint128_t const mpaper_ratio = paper::uint128_t ("1000000000000000000000"); // 10^21
paper::uint128_t const upaper_ratio = paper::uint128_t ("1000000000000000000"); // 10^18
class mdb_env
{
public:
	mdb_env (bool &, boost::filesystem::path const &);
	~mdb_env ();
	operator MDB_env * () const;
	void add_transaction ();
	void remove_transaction ();
	MDB_env * environment;
	std::mutex lock;
	std::condition_variable open_notify;
	unsigned open_transactions;
	unsigned transaction_iteration;
	std::condition_variable resize_notify;
	bool resizing;
};
class mdb_val
{
public:
	mdb_val (size_t, void *);
	operator MDB_val * () const;
	operator MDB_val const & () const;
	MDB_val value;
};
class transaction
{
public:
	transaction (paper::mdb_env &, MDB_txn *, bool);
	~transaction ();
	operator MDB_txn * () const;
	MDB_txn * handle;
	paper::mdb_env & environment;
};
union uint128_union
{
public:
	uint128_union () = default;
	uint128_union (std::string const &);
	uint128_union (uint64_t);
	uint128_union (paper::uint128_union const &) = default;
	uint128_union (paper::uint128_t const &);
	bool operator == (paper::uint128_union const &) const;
	void encode_hex (std::string &) const;
	bool decode_hex (std::string const &);
	void encode_dec (std::string &) const;
	bool decode_dec (std::string const &);
	paper::uint128_t number () const;
	void clear ();
	bool is_zero () const;
	paper::mdb_val val () const;
	std::string to_string () const;
	std::array <uint8_t, 16> bytes;
	std::array <char, 16> chars;
	std::array <uint32_t, 4> dwords;
	std::array <uint64_t, 2> qwords;
};
// Balances are 128 bit.
using amount = uint128_union;
union uint256_union
{
	uint256_union () = default;
	uint256_union (std::string const &);
	uint256_union (uint64_t, uint64_t = 0, uint64_t = 0, uint64_t = 0);
	uint256_union (paper::uint256_t const &);
	uint256_union (paper::uint256_union const &, paper::uint256_union const &, uint128_union const &);
	uint256_union (MDB_val const &);
	uint256_union prv (uint256_union const &, uint128_union const &) const;
	uint256_union & operator ^= (paper::uint256_union const &);
	uint256_union operator ^ (paper::uint256_union const &) const;
	bool operator == (paper::uint256_union const &) const;
	bool operator != (paper::uint256_union const &) const;
	bool operator < (paper::uint256_union const &) const;
	paper::mdb_val val () const;
	void encode_hex (std::string &) const;
	bool decode_hex (std::string const &);
	void encode_dec (std::string &) const;
	bool decode_dec (std::string const &);
	void encode_base58check (std::string &) const;
	std::string to_base58check () const;
	bool decode_base58check (std::string const &);
	std::array <uint8_t, 32> bytes;
	std::array <char, 32> chars;
	std::array <uint64_t, 4> qwords;
	std::array <uint128_union, 2> owords;
	void clear ();
	bool is_zero () const;
	std::string to_string () const;
	paper::uint256_t number () const;
};
// All keys and hashes are 256 bit.
using block_hash = uint256_union;
using account = uint256_union;
using public_key = uint256_union;
using private_key = uint256_union;
using secret_key = uint256_union;
using checksum = uint256_union;
union uint512_union
{
	uint512_union () = default;
	uint512_union (paper::uint512_t const &);
	bool operator == (paper::uint512_union const &) const;
	bool operator != (paper::uint512_union const &) const;
	paper::uint512_union & operator ^= (paper::uint512_union const &);
	void encode_hex (std::string &) const;
	bool decode_hex (std::string const &);
	std::array <uint8_t, 64> bytes;
	std::array <uint32_t, 16> dwords;
	std::array <uint64_t, 8> qwords;
	std::array <uint256_union, 2> uint256s;
	void clear ();
	boost::multiprecision::uint512_t number () const;
};
// Only signatures are 512 bit.
using signature = uint512_union;
paper::uint512_union sign_message (paper::private_key const &, paper::public_key const &, paper::uint256_union const &);
bool validate_message (paper::public_key const &, paper::uint256_union const &, paper::uint512_union const &);
}