#include <paper/utility.hpp>

#include <cryptopp/aes.h>
#include <cryptopp/modes.h>

#include <ed25519-donna/ed25519.h>

#include <liblmdb/lmdb.h>

CryptoPP::AutoSeededRandomPool paper::random_pool;

boost::filesystem::path paper::unique_path ()
{
	auto result (working_path () / boost::filesystem::unique_path ());
	return result;
}

std::string paper::to_string_hex (uint64_t value_a)
{
    std::stringstream stream;
    stream << std::hex << std::noshowbase << std::setw (16) << std::setfill ('0');
    stream << value_a;
    return stream.str ();
}

bool paper::from_string_hex (std::string const & value_a, uint64_t & target_a)
{
    auto result (value_a.size () == 8);
    if (!result)
    {
        std::stringstream stream (value_a);
        stream << std::hex << std::noshowbase;
        uint64_t number_l;
        try
        {
            stream >> number_l;
            target_a = number_l;
        }
        catch (std::runtime_error &)
        {
            result = true;
        }
    }
    return result;
}

paper::mdb_env::mdb_env (bool & error_a, boost::filesystem::path const & path_a) :
open_transactions (0),
transaction_iteration (0),
resizing (false)
{
	boost::system::error_code error;
	if (path_a.has_parent_path ())
	{
		boost::filesystem::create_directories (path_a.parent_path (), error);
		if (!error)
		{
			auto status1 (mdb_env_create (&environment));
			assert (status1 == 0);
			auto status2 (mdb_env_set_maxdbs (environment, 128));
			assert (status2 == 0);
			auto status3 (mdb_env_set_mapsize (environment, database_size_increment));
			assert (status3 == 0);
			auto status4 (mdb_env_open (environment, path_a.string ().c_str (), MDB_NOSUBDIR, 00600));
			error_a = status4 != 0;
		}
		else
		{
			error_a = true;
			environment = nullptr;
		}
	}
	else
	{
		error_a = true;
		environment = nullptr;
	}
}

paper::mdb_env::~mdb_env ()
{
	if (environment != nullptr)
	{
		mdb_env_close (environment);
	}
}

paper::mdb_env::operator MDB_env * () const
{
	return environment;
}

void paper::mdb_env::add_transaction ()
{
	std::unique_lock <std::mutex> lock_l (lock);
	while (resizing)
	{
		resize_notify.wait (lock_l);
	}
	if ((transaction_iteration % paper::database_check_interval) == 0)
	{
		MDB_stat stats;
		mdb_env_stat (environment, &stats);
		MDB_envinfo info;
		mdb_env_info (environment, &info);
		size_t load (info.me_last_pgno * stats.ms_psize);
		auto slack (info.me_mapsize - load);
		if (slack < (paper::database_size_increment / 4))
		{
			resizing = true;
			while (open_transactions > 0)
			{
				open_notify.wait (lock_l);
			}
			auto next_size (((info.me_mapsize / database_size_increment) + 1) * database_size_increment);
			mdb_env_set_mapsize (environment, next_size);
			resizing = false;
			resize_notify.notify_all ();
		}
	}
	++transaction_iteration;
	++open_transactions;
}

void paper::mdb_env::remove_transaction ()
{
	std::lock_guard <std::mutex> lock_l (lock);
	--open_transactions;
	open_notify.notify_all ();
}

paper::mdb_val::mdb_val (size_t size_a, void * data_a) :
value ({size_a, data_a})
{
}

paper::mdb_val::operator MDB_val * () const
{
	// Allow passing a temporary to a non-c++ function which doesn't have constness
	return const_cast <MDB_val *> (&value);
};

paper::mdb_val::operator MDB_val const & () const
{
	return value;
}

paper::transaction::transaction (paper::mdb_env & environment_a, MDB_txn * parent_a, bool write) :
environment (environment_a)
{
	environment_a.add_transaction ();
	auto status (mdb_txn_begin (environment_a, parent_a, write ? 0 : MDB_RDONLY, &handle));
	assert (status == 0);
}

paper::transaction::~transaction ()
{
	auto status (mdb_txn_commit (handle));
	environment.remove_transaction ();
	assert (status == 0);
}

paper::transaction::operator MDB_txn * () const
{
	return handle;
}

paper::uint128_union::uint128_union (std::string const & string_a)
{
	decode_hex (string_a);
}

paper::uint128_union::uint128_union (uint64_t value_a)
{
	*this = paper::uint128_t (value_a);
}

paper::uint128_union::uint128_union (paper::uint128_t const & value_a)
{
    boost::multiprecision::uint128_t number_l (value_a);
	for (auto i (bytes.rbegin ()), n (bytes.rend ()); i != n; ++i)
	{
		*i = ((number_l) & 0xff).convert_to <uint8_t> ();
		number_l >>= 8;
	}
}

bool paper::uint128_union::operator == (paper::uint128_union const & other_a) const
{
    return qwords [0] == other_a.qwords [0] && qwords [1] == other_a.qwords [1];
}

paper::uint128_t paper::uint128_union::number () const
{
    boost::multiprecision::uint128_t result;
	auto shift (0);
	for (auto i (bytes.begin ()), n (bytes.end ()); i != n; ++i)
	{
		result <<= shift;
		result |= *i;
		shift = 8;
	}
    return result;
}

void paper::uint128_union::encode_hex (std::string & text) const
{
    assert (text.empty ());
    std::stringstream stream;
    stream << std::hex << std::noshowbase << std::setw (32) << std::setfill ('0');
    stream << number ();
    text = stream.str ();
}

bool paper::uint128_union::decode_hex (std::string const & text)
{
    auto result (text.size () > 32);
    if (!result)
    {
        std::stringstream stream (text);
        stream << std::hex << std::noshowbase;
        paper::uint128_t number_l;
        try
        {
            stream >> number_l;
            *this = number_l;
        }
        catch (std::runtime_error &)
        {
            result = true;
        }
    }
    return result;
}

void paper::uint128_union::encode_dec (std::string & text) const
{
    assert (text.empty ());
    std::stringstream stream;
    stream << std::dec << std::noshowbase;
    stream << number ();
    text = stream.str ();
}

bool paper::uint128_union::decode_dec (std::string const & text)
{
    auto result (text.size () > 39);
    if (!result)
    {
        std::stringstream stream (text);
        stream << std::dec << std::noshowbase;
        paper::uint128_t number_l;
        try
        {
            stream >> number_l;
            *this = number_l;
        }
        catch (std::runtime_error &)
        {
            result = true;
        }
    }
    return result;
}

void paper::uint128_union::clear ()
{
    qwords.fill (0);
}

bool paper::uint128_union::is_zero () const
{
    return qwords [0] == 0 && qwords [1] == 0;
}

paper::mdb_val paper::uint128_union::val () const
{
	return paper::mdb_val (sizeof (*this), const_cast <paper::uint128_union *> (this));
}

std::string paper::uint128_union::to_string () const
{
	std::string result;
	encode_hex (result);
	return result;
}

bool paper::uint256_union::operator == (paper::uint256_union const & other_a) const
{
	return bytes == other_a.bytes;
}

// Construct a uint256_union = AES_ENC_CTR (cleartext, key, iv)
paper::uint256_union::uint256_union (paper::private_key const & cleartext, paper::secret_key const & key, uint128_union const & iv)
{
	paper::uint256_union exponent (cleartext);
	CryptoPP::AES::Encryption alg (key.bytes.data (), sizeof (key.bytes));
    CryptoPP::CTR_Mode_ExternalCipher::Encryption enc (alg, iv.bytes.data ());
	enc.ProcessData (bytes.data (), exponent.bytes.data (), sizeof (exponent.bytes));
}

paper::uint256_union::uint256_union (MDB_val const & val_a)
{
	assert (val_a.mv_size == sizeof (*this));
	static_assert (sizeof (bytes) == sizeof (*this), "Class not packed");
	std::copy (reinterpret_cast <uint8_t const *> (val_a.mv_data), reinterpret_cast <uint8_t const *> (val_a.mv_data) + sizeof (*this), bytes.data ());
}

// Return a uint256_union = AES_DEC_CTR (this, key, iv)
paper::private_key paper::uint256_union::prv (paper::secret_key const & key_a, uint128_union const & iv) const
{
	CryptoPP::AES::Encryption alg (key_a.bytes.data (), sizeof (key_a.bytes));
	CryptoPP::CTR_Mode_ExternalCipher::Decryption dec (alg, iv.bytes.data ());
	paper::private_key result;
	dec.ProcessData (result.bytes.data (), bytes.data (), sizeof (bytes));
	return result;
}

bool paper::uint256_union::is_zero () const
{
    return qwords [0] == 0 && qwords [1] == 0 && qwords [2] == 0 && qwords [3] == 0;
}

std::string paper::uint256_union::to_string () const
{
    std::string result;
    encode_hex (result);
    return result;
}

bool paper::uint256_union::operator < (paper::uint256_union const & other_a) const
{
    return number () < other_a.number ();
}

paper::uint256_union & paper::uint256_union::operator ^= (paper::uint256_union const & other_a)
{
    auto j (other_a.qwords.begin ());
    for (auto i (qwords.begin ()), n (qwords.end ()); i != n; ++i, ++j)
    {
        *i ^= *j;
    }
    return *this;
}

paper::uint256_union paper::uint256_union::operator ^ (paper::uint256_union const & other_a) const
{
    paper::uint256_union result;
    auto k (result.qwords.begin ());
    for (auto i (qwords.begin ()), j (other_a.qwords.begin ()), n (qwords.end ()); i != n; ++i, ++j, ++k)
    {
        *k = *i ^ *j;
    }
    return result;
}

paper::uint256_union::uint256_union (std::string const & hex_a)
{
    decode_hex (hex_a);
}

void paper::uint256_union::clear ()
{
    qwords.fill (0);
}

paper::uint256_t paper::uint256_union::number () const
{
    boost::multiprecision::uint256_t result;
	auto shift (0);
	for (auto i (bytes.begin ()), n (bytes.end ()); i != n; ++i)
	{
		result <<= shift;
		result |= *i;
		shift = 8;
	}
    return result;
}

paper::mdb_val paper::uint256_union::val () const
{
	return paper::mdb_val (bytes.size (), const_cast <uint8_t *> (bytes.data ()));
}

void paper::uint256_union::encode_hex (std::string & text) const
{
    assert (text.empty ());
    std::stringstream stream;
    stream << std::hex << std::noshowbase << std::setw (64) << std::setfill ('0');
    stream << number ();
    text = stream.str ();
}

bool paper::uint256_union::decode_hex (std::string const & text)
{
    auto result (false);
    if (!text.empty ())
    {
        if (text.size () <= 64)
        {
            std::stringstream stream (text);
            stream << std::hex << std::noshowbase;
            paper::uint256_t number_l;
            try
            {
                stream >> number_l;
                *this = number_l;
            }
            catch (std::runtime_error &)
            {
                result = true;
            }
        }
        else
        {
            result = true;
        }
    }
    else
    {
        result = true;
    }
    return result;
}

void paper::uint256_union::encode_dec (std::string & text) const
{
    assert (text.empty ());
    std::stringstream stream;
    stream << std::dec << std::noshowbase;
    stream << number ();
    text = stream.str ();
}

bool paper::uint256_union::decode_dec (std::string const & text)
{
    auto result (text.size () > 78);
    if (!result)
    {
        std::stringstream stream (text);
        stream << std::dec << std::noshowbase;
        paper::uint256_t number_l;
        try
        {
            stream >> number_l;
            *this = number_l;
        }
        catch (std::runtime_error &)
        {
            result = true;
        }
    }
    return result;
}

paper::uint256_union::uint256_union (uint64_t value0, uint64_t value1, uint64_t value2, uint64_t value3)
{
    qwords [0] = value0;
    qwords [1] = value1;
    qwords [2] = value2;
    qwords [3] = value3;
}

bool paper::uint256_union::operator != (paper::uint256_union const & other_a) const
{
    return ! (*this == other_a);
}

// Base58check is an encoding using [0-9][a-z][A-Z] excluding characters that can be confused
// Base58check also has a 32bit error correction code.
namespace
{
    char const * base58_lookup ("123456789ABCDEFGHJKLMNPQRSTUVWXYZabcdefghijkmnopqrstuvwxyz");
    char const * base58_reverse ("~012345678~~~~~~~9:;<=>?@~ABCDE~FGHIJKLMNOP~~~~~~QRSTUVWXYZ[~\\]^_`abcdefghi");
    char base58_encode (uint8_t value)
    {
        assert (value < 58);
        auto result (base58_lookup [value]);
        return result;
    }
    uint8_t base58_decode (char value)
    {
        auto result (base58_reverse [value - 0x30] - 0x30);
        return result;
    }
}

void paper::uint256_union::encode_base58check (std::string & destination_a) const
{
    assert (destination_a.empty ());
    destination_a.reserve (50);
    uint32_t check;
    blake2b_state hash;
	blake2b_init (&hash, sizeof (check));
    blake2b_update (&hash, bytes.data (), sizeof (bytes));
    blake2b_final (&hash, reinterpret_cast <uint8_t *> (&check), sizeof (check));
    paper::uint512_t number_l (number ());
    number_l |= paper::uint512_t (check) << 256;
    number_l |= paper::uint512_t (13) << (256 + 32);
    while (!number_l.is_zero ())
    {
        auto r ((number_l % 58).convert_to <uint8_t> ());
        number_l /= 58;
        destination_a.push_back (base58_encode (r));
    }
    std::reverse (destination_a.begin (), destination_a.end ());
}

std::string paper::uint256_union::to_base58check () const
{
	std::string result;
	encode_base58check (result);
	return result;
}

bool paper::uint256_union::decode_base58check (std::string const & source_a)
{
    auto result (source_a.size () != 50);
    if (!result)
    {
        paper::uint512_t number_l;
        for (auto i (source_a.begin ()), j (source_a.end ()); !result && i != j; ++i)
        {
            uint8_t byte (base58_decode (*i));
            result = byte == '~';
            if (!result)
            {
                number_l *= 58;
                number_l += byte;
            }
        }
        if (!result)
        {
            *this = number_l.convert_to <paper::uint256_t> ();
            uint32_t check ((number_l >> 256).convert_to <uint32_t> ());
            result = (number_l >> (256 + 32)) != 13;
            if (!result)
            {
                uint32_t validation;
                blake2b_state hash;
				blake2b_init (&hash, sizeof (validation));
                blake2b_update (&hash, bytes.data (), sizeof (bytes));
                blake2b_final (&hash, reinterpret_cast <uint8_t *> (&validation), sizeof (validation));
                result = check != validation;
            }
        }
    }
    return result;
}

paper::uint256_union::uint256_union (paper::uint256_t const & number_a)
{
    boost::multiprecision::uint256_t number_l (number_a);
	for (auto i (bytes.rbegin ()), n (bytes.rend ()); i != n; ++i)
	{
		*i = ((number_l) & 0xff).convert_to <uint8_t> ();
		number_l >>= 8;
	}
}

bool paper::uint512_union::operator == (paper::uint512_union const & other_a) const
{
	return bytes == other_a.bytes;
}

paper::uint512_union::uint512_union (boost::multiprecision::uint512_t const & number_a)
{
    boost::multiprecision::uint512_t number_l (number_a);
	for (auto i (bytes.rbegin ()), n (bytes.rend ()); i != n; ++i)
	{
		*i = ((number_l) & 0xff).convert_to <uint8_t> ();
		number_l >>= 8;
	}
}

void paper::uint512_union::clear ()
{
    bytes.fill (0);
}

boost::multiprecision::uint512_t paper::uint512_union::number () const
{
    boost::multiprecision::uint512_t result;
	auto shift (0);
	for (auto i (bytes.begin ()), n (bytes.end ()); i != n; ++i)
	{
		result <<= shift;
		result |= *i;
		shift = 8;
	}
    return result;
}

void paper::uint512_union::encode_hex (std::string & text) const
{
    assert (text.empty ());
    std::stringstream stream;
    stream << std::hex << std::noshowbase << std::setw (128) << std::setfill ('0');
    stream << number ();
    text = stream.str ();
}

bool paper::uint512_union::decode_hex (std::string const & text)
{
    auto result (text.size () > 128);
    if (!result)
    {
        std::stringstream stream (text);
        stream << std::hex << std::noshowbase;
        paper::uint512_t number_l;
        try
        {
            stream >> number_l;
            *this = number_l;
        }
        catch (std::runtime_error &)
        {
            result = true;
        }
    }
    return result;
}

bool paper::uint512_union::operator != (paper::uint512_union const & other_a) const
{
    return ! (*this == other_a);
}

paper::uint512_union & paper::uint512_union::operator ^= (paper::uint512_union const & other_a)
{
    uint256s [0] ^= other_a.uint256s [0];
    uint256s [1] ^= other_a.uint256s [1];
    return *this;
}

extern "C"
{
#include <ed25519-donna/ed25519-hash-custom.h>
void ed25519_randombytes_unsafe (void * out, size_t outlen)
{
    paper::random_pool.GenerateBlock (reinterpret_cast <uint8_t *> (out), outlen);
}
void ed25519_hash_init (ed25519_hash_context * ctx)
{
    ctx->blake2 = new blake2b_state;
	blake2b_init (reinterpret_cast <blake2b_state *> (ctx->blake2), 64);
}

void ed25519_hash_update (ed25519_hash_context * ctx, uint8_t const * in, size_t inlen)
{
    blake2b_update (reinterpret_cast <blake2b_state *> (ctx->blake2), in, inlen);
}

void ed25519_hash_final (ed25519_hash_context * ctx, uint8_t * out)
{
    blake2b_final (reinterpret_cast <blake2b_state *> (ctx->blake2), out, 64);
    delete reinterpret_cast <blake2b_state *> (ctx->blake2);
}

void ed25519_hash (uint8_t * out, uint8_t const * in, size_t inlen)
{
    ed25519_hash_context ctx;
    ed25519_hash_init (&ctx);
    ed25519_hash_update (&ctx, in, inlen);
    ed25519_hash_final (&ctx, out);
}
}

paper::uint512_union paper::sign_message (paper::private_key const & private_key, paper::public_key const & public_key, paper::uint256_union const & message)
{
	paper::uint512_union result;
    ed25519_sign (message.bytes.data (), sizeof (message.bytes), private_key.bytes.data (), public_key.bytes.data (), result.bytes.data ());
	return result;
}

bool paper::validate_message (paper::public_key const & public_key, paper::uint256_union const & message, paper::uint512_union const & signature)
{
    auto result (0 != ed25519_sign_open (message.bytes.data (), sizeof (message.bytes), public_key.bytes.data (), signature.bytes.data ()));
    return result;
}
