#include <paper/node.hpp>

#include <ed25519-donna/ed25519.h>

#include <unordered_set>
#include <memory>
#include <sstream>

#include <boost/log/expressions.hpp>
#include <boost/log/utility/setup/common_attributes.hpp>
#include <boost/log/utility/setup/console.hpp>
#include <boost/log/utility/setup/file.hpp>
#include <boost/property_tree/ptree.hpp>
#include <boost/property_tree/json_parser.hpp>

#include <thread>

paper::message_parser::message_parser (paper::message_visitor & visitor_a) :
visitor (visitor_a),
error (false),
insufficient_work (false)
{
}

void paper::message_parser::deserialize_buffer (uint8_t const * buffer_a, size_t size_a)
{
    error = false;
    paper::bufferstream header_stream (buffer_a, size_a);
    uint8_t version_max;
    uint8_t version_using;
    uint8_t version_min;
    paper::message_type type;
    std::bitset <16> extensions;
    if (!paper::message::read_header (header_stream, version_max, version_using, version_min, type, extensions))
    {
        switch (type)
        {
            case paper::message_type::keepalive:
            {
                deserialize_keepalive (buffer_a, size_a);
                break;
            }
            case paper::message_type::publish:
            {
                deserialize_publish (buffer_a, size_a);
                break;
            }
            case paper::message_type::confirm_req:
            {
                deserialize_confirm_req (buffer_a, size_a);
                break;
            }
            case paper::message_type::confirm_ack:
            {
                deserialize_confirm_ack (buffer_a, size_a);
                break;
            }
            default:
            {
                error = true;
                break;
            }
        }
    }
    else
    {
        error = true;
    }
}

void paper::message_parser::deserialize_keepalive (uint8_t const * buffer_a, size_t size_a)
{
    paper::keepalive incoming;
    paper::bufferstream stream (buffer_a, size_a);
    auto error_l (incoming.deserialize (stream));
    if (!error_l && at_end (stream))
    {
        visitor.keepalive (incoming);
    }
    else
    {
        error = true;
    }
}

void paper::message_parser::deserialize_publish (uint8_t const * buffer_a, size_t size_a)
{
    paper::publish incoming;
    paper::bufferstream stream (buffer_a, size_a);
    auto error_l (incoming.deserialize (stream));
    if (!error_l && at_end (stream))
    {
        if (!paper::work_validate (*incoming.block))
        {
            visitor.publish (incoming);
        }
        else
        {
            insufficient_work = true;
        }
    }
    else
    {
        error = true;
    }
}

void paper::message_parser::deserialize_confirm_req (uint8_t const * buffer_a, size_t size_a)
{
    paper::confirm_req incoming;
    paper::bufferstream stream (buffer_a, size_a);
    auto error_l (incoming.deserialize (stream));
    if (!error_l && at_end (stream))
    {
        if (!paper::work_validate (*incoming.block))
        {
            visitor.confirm_req (incoming);
        }
        else
        {
            insufficient_work = true;
        }
    }
    else
    {
        error = true;
    }
}

void paper::message_parser::deserialize_confirm_ack (uint8_t const * buffer_a, size_t size_a)
{
	bool error_l;
    paper::bufferstream stream (buffer_a, size_a);
    paper::confirm_ack incoming (error_l, stream);
    if (!error_l && at_end (stream))
    {
        if (!paper::work_validate (*incoming.vote.block))
        {
            visitor.confirm_ack (incoming);
        }
        else
        {
            insufficient_work = true;
        }
    }
    else
    {
        error = true;
    }
}

bool paper::message_parser::at_end (paper::bufferstream & stream_a)
{
    uint8_t junk;
    auto end (paper::read (stream_a, junk));
    return end;
}

double constexpr paper::node::price_max;
double constexpr paper::node::free_cutoff;
std::chrono::seconds constexpr paper::node::period;
std::chrono::seconds constexpr paper::node::cutoff;
std::chrono::minutes constexpr paper::node::backup_interval;
// Cutoff for receiving a block if no forks are observed.
std::chrono::milliseconds const paper::confirm_wait = paper_network == paper_networks::paper_test_network ? std::chrono::milliseconds (0) : std::chrono::milliseconds (5000);

paper::network::network (boost::asio::io_service & service_a, uint16_t port, paper::node & node_a) :
socket (service_a, boost::asio::ip::udp::endpoint (boost::asio::ip::address_v6::any (), port)),
service (service_a),
resolver (service_a),
node (node_a),
bad_sender_count (0),
on (true),
keepalive_count (0),
publish_count (0),
confirm_req_count (0),
confirm_ack_count (0),
insufficient_work_count (0),
error_count (0)
{
}

void paper::network::receive ()
{
    if (node.config.logging.network_packet_logging ())
    {
        BOOST_LOG (node.log) << "Receiving packet";
    }
    std::unique_lock <std::mutex> lock (socket_mutex);
    socket.async_receive_from (boost::asio::buffer (buffer.data (), buffer.size ()), remote,
        [this] (boost::system::error_code const & error, size_t size_a)
        {
            receive_action (error, size_a);
        });
}

void paper::network::stop ()
{
    on = false;
    socket.close ();
    resolver.cancel ();
}

void paper::network::send_keepalive (paper::endpoint const & endpoint_a)
{
    assert (endpoint_a.address ().is_v6 ());
    paper::keepalive message;
    node.peers.random_fill (message.peers);
    std::shared_ptr <std::vector <uint8_t>> bytes (new std::vector <uint8_t>);
    {
        paper::vectorstream stream (*bytes);
        message.serialize (stream);
    }
    if (node.config.logging.network_keepalive_logging ())
    {
        BOOST_LOG (node.log) << boost::str (boost::format ("Keepalive req sent from %1% to %2%") % endpoint () % endpoint_a);
    }
    auto node_l (node.shared ());
    send_buffer (bytes->data (), bytes->size (), endpoint_a, 0, [bytes, node_l, endpoint_a] (boost::system::error_code const & ec, size_t)
        {
            if (node_l->config.logging.network_logging ())
            {
                if (ec)
                {
                    BOOST_LOG (node_l->log) << boost::str (boost::format ("Error sending keepalive from %1% to %2% %3%") % node_l->network.endpoint () % endpoint_a % ec.message ());
                }
            }
        });
}

void paper::node::keepalive (std::string const & address_a, uint16_t port_a)
{
	auto node_l (shared_from_this ());
	network.resolver.async_resolve (boost::asio::ip::udp::resolver::query (address_a, std::to_string (port_a), boost::asio::ip::resolver_query_base::all_matching | boost::asio::ip::resolver_query_base::v4_mapped), [node_l, address_a] (boost::system::error_code const & ec, boost::asio::ip::udp::resolver::iterator i_a)
	{
		if (!ec)
		{
			for (auto i (i_a), n (boost::asio::ip::udp::resolver::iterator {}); i != n; ++i)
			{
				node_l->send_keepalive (i->endpoint ());
			}
		}
		else
		{
			BOOST_LOG (node_l->log) << boost::str (boost::format ("Error resolving address: %1%, %2%") % address_a % ec.message ());
		}
	});
}

void paper::network::republish_block (std::unique_ptr <paper::block> block, size_t rebroadcast_a)
{
	auto hash (block->hash ());
    auto list (node.peers.list ());
	// If we're a representative, broadcast a signed confirm, otherwise an unsigned publish
    if (!confirm_broadcast (list, block->clone (), 0, rebroadcast_a))
    {
        paper::publish message (std::move (block));
        std::shared_ptr <std::vector <uint8_t>> bytes (new std::vector <uint8_t>);
        {
            paper::vectorstream stream (*bytes);
            message.serialize (stream);
        }
        auto node_l (node.shared ());
        for (auto i (list.begin ()), n (list.end ()); i != n; ++i)
        {
			if (!node.peers.knows_about (i->endpoint, hash))
			{
				if (node.config.logging.network_publish_logging ())
				{
					BOOST_LOG (node.log) << boost::str (boost::format ("Publish %1% to %2%") % hash.to_string () % i->endpoint);
				}
				send_buffer (bytes->data (), bytes->size (), i->endpoint, rebroadcast_a, [bytes, node_l] (boost::system::error_code const & ec, size_t size)
					{
						if (node_l->config.logging.network_logging ())
						{
							if (ec)
							{
								BOOST_LOG (node_l->log) << boost::str (boost::format ("Error sending publish: %1% from %2%") % ec.message () % node_l->network.endpoint ());
							}
						}
					});
			}
        }
		BOOST_LOG (node.log) << boost::str (boost::format ("Block %1% was published from %2%") % hash.to_string () % endpoint ());
    }
	else
	{
		BOOST_LOG (node.log) << boost::str (boost::format ("Block %1% was confirmed from %2%") % hash.to_string () % endpoint ());
	}
}

void paper::network::broadcast_confirm_req (paper::block const & block_a)
{
	auto list (node.peers.list ());
	for (auto i (list.begin ()), j (list.end ()); i != j; ++i)
	{
		node.network.send_confirm_req (i->endpoint, block_a);
	}
}

void paper::network::send_confirm_req (boost::asio::ip::udp::endpoint const & endpoint_a, paper::block const & block)
{
    paper::confirm_req message (block.clone ());
    std::shared_ptr <std::vector <uint8_t>> bytes (new std::vector <uint8_t>);
    {
        paper::vectorstream stream (*bytes);
        message.serialize (stream);
    }
    if (node.config.logging.network_logging ())
    {
        BOOST_LOG (node.log) << boost::str (boost::format ("Sending confirm req to %1%") % endpoint_a);
    }
    auto node_l (node.shared ());
    send_buffer (bytes->data (), bytes->size (), endpoint_a, 0, [bytes, node_l] (boost::system::error_code const & ec, size_t size)
        {
            if (node_l->config.logging.network_logging ())
            {
                if (ec)
                {
                    BOOST_LOG (node_l->log) << boost::str (boost::format ("Error sending confirm request: %1%") % ec.message ());
                }
            }
        });
}

namespace
{
class network_message_visitor : public paper::message_visitor
{
public:
    network_message_visitor (paper::node & node_a, paper::endpoint const & sender_a) :
    node (node_a),
    sender (sender_a)
    {
    }
    void keepalive (paper::keepalive const & message_a) override
    {
        if (node.config.logging.network_keepalive_logging ())
        {
            BOOST_LOG (node.log) << boost::str (boost::format ("Received keepalive message from %1%") % sender);
        }
        ++node.network.keepalive_count;
        node.peers.contacted (sender);
        node.network.merge_peers (message_a.peers);
    }
    void publish (paper::publish const & message_a) override
    {
        if (node.config.logging.network_message_logging ())
        {
            BOOST_LOG (node.log) << boost::str (boost::format ("Received publish message from %1%") % sender);
        }
        ++node.network.publish_count;
        node.peers.contacted (sender);
        node.peers.insert (sender, message_a.block->hash ());
        node.process_receive_republish (message_a.block->clone (), 0);
    }
    void confirm_req (paper::confirm_req const & message_a) override
    {
        if (node.config.logging.network_message_logging ())
        {
            BOOST_LOG (node.log) << boost::str (boost::format ("Received confirm_req message from %1%") % sender);
        }
        ++node.network.confirm_req_count;
        node.peers.contacted (sender);
        node.peers.insert (sender, message_a.block->hash ());
        node.process_receive_republish (message_a.block->clone (), 0);
		bool exists;
		{
			paper::transaction transaction (node.store.environment, nullptr, false);
			exists = node.store.block_exists (transaction, message_a.block->hash ());
		}
        if (exists)
        {
            node.process_confirmation (*message_a.block, sender);
        }
    }
    void confirm_ack (paper::confirm_ack const & message_a) override
    {
        if (node.config.logging.network_message_logging ())
        {
            BOOST_LOG (node.log) << boost::str (boost::format ("Received confirm_ack message from %1%") % sender);
        }
        ++node.network.confirm_ack_count;
        node.peers.contacted (sender);
        node.peers.insert (sender, message_a.vote.block->hash ());
        node.process_receive_republish (message_a.vote.block->clone (), 0);
        node.vote (message_a.vote);
    }
    void bulk_pull (paper::bulk_pull const &) override
    {
        assert (false);
    }
    void bulk_push (paper::bulk_push const &) override
    {
        assert (false);
    }
    void frontier_req (paper::frontier_req const &) override
    {
        assert (false);
    }
    paper::node & node;
    paper::endpoint sender;
};
}

void paper::network::receive_action (boost::system::error_code const & error, size_t size_a)
{
    if (!error && on)
    {
        if (!paper::reserved_address (remote) && remote != endpoint ())
        {
            network_message_visitor visitor (node, remote);
            paper::message_parser parser (visitor);
            parser.deserialize_buffer (buffer.data (), size_a);
            if (parser.error)
            {
                ++error_count;
            }
            else if (parser.insufficient_work)
            {
                if (node.config.logging.insufficient_work_logging ())
                {
                    BOOST_LOG (node.log) << "Insufficient work in message";
                }
                ++insufficient_work_count;
            }
        }
        else
        {
            if (node.config.logging.network_logging ())
            {
                BOOST_LOG (node.log) << "Reserved sender";
            }
            ++bad_sender_count;
        }
        receive ();
    }
    else
    {
        if (node.config.logging.network_logging ())
        {
            BOOST_LOG (node.log) << boost::str (boost::format ("Receive error: %1%") % error.message ());
        }
        node.service.add (std::chrono::system_clock::now () + std::chrono::seconds (5), [this] () { receive (); });
    }
}

// Send keepalives to all the peers we've been notified of
void paper::network::merge_peers (std::array <paper::endpoint, 8> const & peers_a)
{
    for (auto i (peers_a.begin ()), j (peers_a.end ()); i != j; ++i)
    {
        if (!node.peers.not_a_peer (*i) && !node.peers.known_peer (*i))
        {
            send_keepalive (*i);
        }
    }
}

paper::publish::publish () :
message (paper::message_type::publish)
{
}

paper::publish::publish (std::unique_ptr <paper::block> block_a) :
message (paper::message_type::publish),
block (std::move (block_a))
{
    block_type_set (block->type ());
}

bool paper::publish::deserialize (paper::stream & stream_a)
{
    auto result (read_header (stream_a, version_max, version_using, version_min, type, extensions));
    assert (!result);
	assert (type == paper::message_type::publish);
    if (!result)
    {
        block = paper::deserialize_block (stream_a, block_type ());
        result = block == nullptr;
    }
    return result;
}

void paper::publish::serialize (paper::stream & stream_a)
{
    assert (block != nullptr);
	write_header (stream_a);
    block->serialize (stream_a);
}

paper::wallet_value::wallet_value (MDB_val const & val_a)
{
	assert (val_a.mv_size == sizeof (*this));
	std::copy (reinterpret_cast <uint8_t const *> (val_a.mv_data), reinterpret_cast <uint8_t const *> (val_a.mv_data) + sizeof (key), key.chars.begin ());
	std::copy (reinterpret_cast <uint8_t const *> (val_a.mv_data) + sizeof (key), reinterpret_cast <uint8_t const *> (val_a.mv_data) + sizeof (key) + sizeof (work), reinterpret_cast <char *> (&work));
}

paper::wallet_value::wallet_value (paper::uint256_union const & value_a) :
key (value_a),
work (0)
{
}

paper::mdb_val paper::wallet_value::val () const
{
	static_assert (sizeof (*this) == sizeof (key) + sizeof (work), "Class not packed");
	return paper::mdb_val (sizeof (*this), const_cast <paper::wallet_value *> (this));
}

paper::uint256_union const paper::wallet_store::version_1 (1);
paper::uint256_union const paper::wallet_store::version_current (version_1);
paper::uint256_union const paper::wallet_store::version_special (0);
paper::uint256_union const paper::wallet_store::salt_special (1);
paper::uint256_union const paper::wallet_store::wallet_key_special (2);
paper::uint256_union const paper::wallet_store::check_special (3);
paper::uint256_union const paper::wallet_store::representative_special (4);
int const paper::wallet_store::special_count (5);

paper::wallet_store::wallet_store (bool & init_a, paper::transaction & transaction_a, std::string const & wallet_a, std::string const & json_a) :
password (0, 1024),
environment (transaction_a.environment)
{
    init_a = false;
    initialize (transaction_a, init_a, wallet_a);
    if (!init_a)
    {
        MDB_val junk;
		assert (mdb_get (transaction_a, handle, version_special.val (), &junk) == MDB_NOTFOUND);
        boost::property_tree::ptree wallet_l;
        std::stringstream istream (json_a);
        boost::property_tree::read_json (istream, wallet_l);
        for (auto i (wallet_l.begin ()), n (wallet_l.end ()); i != n; ++i)
        {
            paper::uint256_union key;
            init_a = key.decode_hex (i->first);
            if (!init_a)
            {
                paper::uint256_union value;
                init_a = value.decode_hex (wallet_l.get <std::string> (i->first));
                if (!init_a)
                {
					entry_put_raw (transaction_a, key, paper::wallet_value (value));
                }
                else
                {
                    init_a = true;
                }
            }
            else
            {
                init_a = true;
            }
        }
		init_a = init_a || mdb_get (transaction_a, handle, version_special.val (), &junk) != 0;
		init_a = init_a || mdb_get (transaction_a, handle, wallet_key_special.val (), & junk) != 0;
		init_a = init_a || mdb_get (transaction_a, handle, salt_special.val (), &junk) != 0;
		init_a = init_a || mdb_get (transaction_a, handle, check_special.val (), &junk) != 0;
		init_a = init_a || mdb_get (transaction_a, handle, representative_special.val (), &junk) != 0;
		password.value_set (0);
    }
}

paper::wallet_store::wallet_store (bool & init_a, paper::transaction & transaction_a, std::string const & wallet_a) :
password (0, 1024),
environment (transaction_a.environment)
{
    init_a = false;
    initialize (transaction_a, init_a, wallet_a);
    if (!init_a)
    {
		int version_status;
		MDB_val version_value;
		version_status = mdb_get (transaction_a, handle, version_special.val (), &version_value);
        if (version_status == MDB_NOTFOUND)
        {
			entry_put_raw (transaction_a, paper::wallet_store::version_special, paper::wallet_value (version_current));
            paper::uint256_union salt_l;
            random_pool.GenerateBlock (salt_l.bytes.data (), salt_l.bytes.size ());
			entry_put_raw (transaction_a, paper::wallet_store::salt_special, paper::wallet_value (salt_l));
            // Wallet key is a fixed random key that encrypts all entries
            paper::uint256_union wallet_key;
            random_pool.GenerateBlock (wallet_key.bytes.data (), sizeof (wallet_key.bytes));
            password.value_set (0);
            // Wallet key is encrypted by the user's password
            paper::uint256_union encrypted (wallet_key, 0, salt_l.owords [0]);
			entry_put_raw (transaction_a, paper::wallet_store::wallet_key_special, paper::wallet_value (encrypted));
            paper::uint256_union zero (0);
            paper::uint256_union check (zero, wallet_key, salt_l.owords [0]);
			entry_put_raw (transaction_a, paper::wallet_store::check_special, paper::wallet_value (check));
            wallet_key.clear ();
			entry_put_raw (transaction_a, paper::wallet_store::representative_special, paper::wallet_value (paper::genesis_account));
        }
        else
        {
            enter_password (transaction_a, "");
        }
    }
}

std::vector <paper::account> paper::wallet_store::accounts (MDB_txn * transaction_a)
{
	std::vector <paper::account> result;
	for (auto i (begin (transaction_a)), n (end ()); i != n; ++i)
	{
		paper::account account (i->first);
		result.push_back (account);
	}
	return result;
}

void paper::wallet_store::initialize (MDB_txn * transaction_a, bool & init_a, std::string const & path_a)
{
	assert (strlen (path_a.c_str ()) == path_a.size ());
	auto error (mdb_dbi_open (transaction_a, path_a.c_str (), MDB_CREATE, &handle));
	init_a = error != 0;
}

bool paper::wallet_store::is_representative (MDB_txn * transaction_a)
{
    return exists (transaction_a, representative (transaction_a));
}

void paper::wallet_store::representative_set (MDB_txn * transaction_a, paper::account const & representative_a)
{
	entry_put_raw (transaction_a, paper::wallet_store::representative_special, paper::wallet_value (representative_a));
}

paper::account paper::wallet_store::representative (MDB_txn * transaction_a)
{
	paper::wallet_value value (entry_get_raw (transaction_a, paper::wallet_store::representative_special));
    return value.key;
}

paper::public_key paper::wallet_store::insert (MDB_txn * transaction_a, paper::private_key const & prv)
{
    paper::public_key pub;
    ed25519_publickey (prv.bytes.data (), pub.bytes.data ());
	entry_put_raw (transaction_a, pub, paper::wallet_value (paper::uint256_union (prv, wallet_key (transaction_a), salt (transaction_a).owords [0])));
	return pub;
}

void paper::wallet_store::erase (MDB_txn * transaction_a, paper::public_key const & pub)
{
	auto status (mdb_del (transaction_a, handle, pub.val (), nullptr));
    assert (status == 0);
}

paper::wallet_value paper::wallet_store::entry_get_raw (MDB_txn * transaction_a, paper::public_key const & pub_a)
{
	paper::wallet_value result;
	MDB_val value;
	auto status (mdb_get (transaction_a, handle, pub_a.val (), &value));
	if (status == 0)
	{
		result = paper::wallet_value (value);
	}
	else
	{
		result.key.clear ();
		result.work = 0;
	}
	return result;
}

void paper::wallet_store::entry_put_raw (MDB_txn * transaction_a, paper::public_key const & pub_a, paper::wallet_value const & entry_a)
{
	auto status (mdb_put (transaction_a, handle, pub_a.val (), entry_a.val (), 0));
	assert (status == 0);
}

bool paper::wallet_store::fetch (MDB_txn * transaction_a, paper::public_key const & pub, paper::private_key & prv)
{
    auto result (false);
	paper::wallet_value value (entry_get_raw (transaction_a, pub));
    if (!value.key.is_zero ())
    {
        prv = value.key.prv (wallet_key (transaction_a), salt (transaction_a).owords [0]);
        paper::public_key compare;
        ed25519_publickey (prv.bytes.data (), compare.bytes.data ());
        if (!(pub == compare))
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

bool paper::wallet_store::exists (MDB_txn * transaction_a, paper::public_key const & pub)
{
    return find (transaction_a, pub) != end ();
}

void paper::wallet_store::serialize_json (MDB_txn * transaction_a, std::string & string_a)
{
    boost::property_tree::ptree tree;
    for (paper::store_iterator i (transaction_a, handle), n (nullptr); i != n; ++i)
    {
        tree.put (paper::uint256_union (i->first).to_string (), paper::wallet_value (i->second).key.to_string ());
    }
    std::stringstream ostream;
    boost::property_tree::write_json (ostream, tree);
    string_a = ostream.str ();
}

void paper::wallet_store::write_backup (MDB_txn * transaction_a, boost::filesystem::path const & path_a)
{
	std::ofstream backup_file;
	backup_file.open (path_a.string ());
	if (!backup_file.fail ())
	{
		std::string json;
		serialize_json (transaction_a, json);
		backup_file << json;
	}
}

bool paper::wallet_store::move (MDB_txn * transaction_a, paper::wallet_store & other_a, std::vector <paper::public_key> const & keys)
{
    assert (valid_password (transaction_a));
    assert (other_a.valid_password (transaction_a));
    auto result (false);
    for (auto i (keys.begin ()), n (keys.end ()); i != n; ++i)
    {
        paper::private_key prv;
        auto error (other_a.fetch (transaction_a, *i, prv));
        result = result | error;
        if (!result)
        {
            insert (transaction_a, prv);
            other_a.erase (transaction_a, *i);
        }
    }
    return result;
}

bool paper::wallet_store::import (MDB_txn * transaction_a, paper::wallet_store & other_a)
{
    assert (valid_password (transaction_a));
    assert (other_a.valid_password (transaction_a));
    auto result (false);
    for (auto i (other_a.begin (transaction_a)), n (end ()); i != n; ++i)
    {
        paper::private_key prv;
        auto error (other_a.fetch (transaction_a, i->first, prv));
        result = result | error;
        if (!result)
        {
            insert (transaction_a, prv);
            other_a.erase (transaction_a, i->first);
        }
    }
    return result;
}

bool paper::wallet_store::work_get (MDB_txn * transaction_a, paper::public_key const & pub_a, uint64_t & work_a)
{
	auto result (false);
	auto entry (entry_get_raw (transaction_a, pub_a));
	if (!entry.key.is_zero ())
	{
		work_a = entry.work;
	}
	else
	{
		result = true;
	}
	return result;
}

void paper::wallet_store::work_put (MDB_txn * transaction_a, paper::public_key const & pub_a, uint64_t work_a)
{
	auto entry (entry_get_raw (transaction_a, pub_a));
	assert (!entry.key.is_zero ());
	entry.work = work_a;
	entry_put_raw (transaction_a, pub_a, entry);
}

paper::wallet::wallet (bool & init_a, paper::transaction & transaction_a, paper::node & node_a, std::string const & wallet_a) :
store (init_a, transaction_a, wallet_a),
node (node_a)
{
}

paper::wallet::wallet (bool & init_a, paper::transaction & transaction_a, paper::node & node_a, std::string const & wallet_a, std::string const & json) :
store (init_a, transaction_a, wallet_a, json),
node (node_a)
{
}

void paper::wallet::enter_initial_password (MDB_txn * transaction_a)
{
	if (store.password.value ().is_zero ())
	{
		if (store.valid_password (transaction_a))
		{
			// Newly created wallets have a zero key
			store.rekey (transaction_a, "");
		}
		else
		{
			store.enter_password (transaction_a, "");
		}
	}
}

paper::public_key paper::wallet::insert (paper::private_key const & key_a)
{
	paper::block_hash root;
	paper::public_key key;
	{
		paper::transaction transaction (store.environment, nullptr, true);
		key = store.insert (transaction, key_a);
		auto this_l (shared_from_this ());
		root = node.ledger.latest_root (transaction, key);
	}
	work_generate (key, root);
	return key;
}

bool paper::wallet::exists (paper::public_key const & account_a)
{
	paper::transaction transaction (store.environment, nullptr, false);
	return store.exists (transaction, account_a);
}

bool paper::wallet::import (std::string const & json_a, std::string const & password_a)
{
	paper::transaction transaction (store.environment, nullptr, true);
	paper::uint256_union id;
	random_pool.GenerateBlock (id.bytes.data (), id.bytes.size ());
	auto error (false);
	paper::wallet_store temp (error, transaction, id.to_string (), json_a);
	if (!error)
	{
		temp.enter_password (transaction, password_a);
		if (temp.valid_password (transaction))
		{
			error = store.import (transaction, temp);
		}
		else
		{
			error = true;
		}
	}
	temp.destroy (transaction);
	return error;
}

void paper::wallet::serialize (std::string & json_a)
{
	paper::transaction transaction (store.environment, nullptr, false);
	store.serialize_json (transaction, json_a);
}

void paper::wallet_store::destroy (MDB_txn * transaction_a)
{
	auto status (mdb_drop (transaction_a, handle, 1));
	assert (status == 0);
}

namespace {
bool check_ownership (paper::wallets & wallets_a, paper::account const & account_a) {
	std::lock_guard <std::mutex> lock (wallets_a.action_mutex);
	return wallets_a.current_actions.find (account_a) == wallets_a.current_actions.end ();
}
}

bool paper::wallet::receive_action (paper::send_block const & send_a, paper::private_key const & prv_a, paper::account const & representative_a)
{
	assert (!check_ownership (node.wallets, send_a.hashables.destination));
    auto hash (send_a.hash ());
    bool result;
	std::unique_ptr <paper::block> block;
	{
		paper::transaction transaction (node.ledger.store.environment, nullptr, false);
		if (node.ledger.store.pending_exists (transaction, hash))
		{
			paper::account_info info;
			auto new_account (node.ledger.store.account_get (transaction, send_a.hashables.destination, info));
			if (!new_account)
			{
				auto receive (new paper::receive_block (info.head, hash, prv_a, send_a.hashables.destination, work_fetch (transaction, send_a.hashables.destination, info.head)));
				block.reset (receive);
			}
			else
			{
				block.reset (new paper::open_block (hash, representative_a, send_a.hashables.destination, prv_a, send_a.hashables.destination, work_fetch (transaction, send_a.hashables.destination, send_a.hashables.destination)));
			}
			result = false;
		}
		else
		{
			result = true;
			// Ledger doesn't have this marked as available to receive anymore
		}
	}
	if (!result)
	{
		assert (block != nullptr);
		node.process_receive_republish (block->clone (), node.config.creation_rebroadcast);
		work_generate (send_a.hashables.destination, block->hash ());
	}
    return result;
}

bool paper::wallet::change_action (paper::account const & source_a, paper::account const & representative_a)
{
	assert (!check_ownership (node.wallets, source_a));
	std::unique_ptr <paper::change_block> block;
	auto result (false);
	{
		paper::transaction transaction (store.environment, nullptr, false);
		result = !store.valid_password (transaction);
		if (!result)
		{
			auto existing (store.find (transaction, source_a));
			if (existing != store.end ())
			{
				if (!node.ledger.latest (transaction, source_a).is_zero ())
				{
					paper::account_info info;
					result = node.ledger.store.account_get (transaction, source_a, info);
					assert (!result);
					paper::private_key prv;
					result = store.fetch (transaction, source_a, prv);
					assert (!result);
					block.reset (new paper::change_block (info.head, representative_a, prv, source_a, work_fetch (transaction, source_a, info.head)));
					prv.clear ();
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
		}
	}
	if (!result)
	{
		assert (block != nullptr);
		node.process_receive_republish (block->clone (), node.config.creation_rebroadcast);
		work_generate (source_a, block->hash ());
	}
	return result;
}

bool paper::wallet::send_action (paper::account const & source_a, paper::account const & account_a, paper::uint128_t const & amount_a)
{
	assert (!check_ownership (node.wallets, source_a));
	std::unique_ptr <paper::send_block> block;
	auto result (false);
	{
		paper::transaction transaction (store.environment, nullptr, false);
		result = !store.valid_password (transaction);
		if (!result)
		{
			auto existing (store.find (transaction, source_a));
			if (existing != store.end ())
			{
				auto balance (node.ledger.account_balance (transaction, source_a));
				if (!balance.is_zero ())
				{
					if (balance >= amount_a)
					{
						paper::account_info info;
						result = node.ledger.store.account_get (transaction, source_a, info);
						assert (!result);
						paper::private_key prv;
						result = store.fetch (transaction, source_a, prv);
						assert (!result);
						block.reset (new paper::send_block (info.head, account_a, balance - amount_a, prv, source_a, work_fetch (transaction, source_a, info.head)));
						prv.clear ();
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
			}
			else
			{
				result = true;
			}
		}
	}
	if (!result)
	{
		assert (block != nullptr);
		node.process_receive_republish (block->clone (), node.config.creation_rebroadcast);
		work_generate (source_a, block->hash ());
	}
	return result;
}

bool paper::wallet::change_sync (paper::account const & source_a, paper::account const & representative_a)
{
	std::mutex complete;
	complete.lock ();
	bool result;
	node.wallets.queue_wallet_action (source_a, [this, source_a, representative_a, &complete, &result] ()
	{
		result = change_action (source_a, representative_a);
		complete.unlock ();
	});
	complete.lock ();
	return result;
}

bool paper::wallet::receive_sync (paper::send_block const & block_a, paper::private_key const & prv_a, paper::account const & account_a)
{
	std::mutex complete;
	complete.lock ();
	bool result;
	node.wallets.queue_wallet_action (block_a.hashables.destination, [this, &block_a, &prv_a, account_a, &result, &complete] ()
	{
		result = receive_action (block_a, prv_a, account_a);
		complete.unlock ();
	});
	complete.lock ();
	return result;
}

bool paper::wallet::send_sync (paper::account const & source_a, paper::account const & account_a, paper::uint128_t const & amount_a)
{
	std::mutex complete;
	complete.lock ();
	bool result;
	node.wallets.queue_wallet_action (source_a, [this, source_a, account_a, amount_a, &complete, &result] ()
	{
		result = send_action (source_a, account_a, amount_a);
		complete.unlock ();
	});
	complete.lock ();
	return result;
}

// Update work for account if latest root is root_a
void paper::wallet::work_update (MDB_txn * transaction_a, paper::account const & account_a, paper::block_hash const & root_a, uint64_t work_a)
{
    assert (!paper::work_validate (root_a, work_a));
    assert (store.exists (transaction_a, account_a));
    auto latest (node.ledger.latest_root (transaction_a, account_a));
    if (latest == root_a)
    {
        BOOST_LOG (node.log) << "Successfully cached work";
        store.work_put (transaction_a, account_a, work_a);
    }
    else
    {
        BOOST_LOG (node.log) << "Cached work no longer valid, discarding";
    }
}

// Fetch work for root_a, use cached value if possible
uint64_t paper::wallet::work_fetch (MDB_txn * transaction_a, paper::account const & account_a, paper::block_hash const & root_a)
{
    uint64_t result;
    auto error (store.work_get (transaction_a, account_a, result));
    if (error)
	{
        result = node.work.generate (root_a);
    }
	else
	{
		if (paper::work_validate (root_a, result))
		{
			BOOST_LOG (node.log) << "Cached work invalid, regenerating";
			result = node.work.generate (root_a);
		}
	}
    return result;
}

namespace
{
class search_action : public std::enable_shared_from_this <search_action>
{
public:
	search_action (std::shared_ptr <paper::wallet> const & wallet_a, MDB_txn * transaction_a) :
	current_block (0),
	wallet (wallet_a)
	{
		for (auto i (wallet_a->store.begin (transaction_a)), n (wallet_a->store.end ()); i != n; ++i)
		{
			keys.insert (i->first);
		}
	}
	void run ()
	{
		BOOST_LOG (wallet->node.log) << "Beginning pending block search";
		paper::account account;
		std::unique_ptr <paper::block> block;
		{
			paper::transaction transaction (wallet->node.store.environment, nullptr, false);
			auto next (current_block.number () + 1);
			current_block = 0;
			for (auto i (wallet->node.store.pending_begin (transaction, next)), n (wallet->node.store.pending_end ()); i != n && block == nullptr; ++i)
			{
				paper::receivable receivable (i->second);
				if (keys.find (receivable.destination) != keys.end ())
				{
					current_block = i->first;
					paper::account_info info;
					wallet->node.store.account_get (transaction, receivable.source, info);
					account = receivable.source;
					BOOST_LOG (wallet->node.log) << boost::str (boost::format ("Found a pending block %1% from account %2% with head %3%") % current_block.to_string () % account.to_string () % info.head.to_string ());
					block = wallet->node.store.block_get (transaction, info.head);
				}
			}
		}
		if (!current_block.is_zero ())
		{
			auto this_l (shared_from_this ());
			wallet->node.conflicts.start (*block, [this_l, account] (paper::block &)
			{
				this_l->receive_all (account);
			}, true);
		}
		else
		{
			BOOST_LOG (wallet->node.log) << "Pending block search complete";
		}
	}
	void receive_all (paper::account const & account_a)
	{
		BOOST_LOG (wallet->node.log) << boost::str (boost::format ("Account %1% confirmed, receiving all blocks") % account_a.to_base58check ());
		paper::block_hash hash (current_block);
		while (!hash.is_zero ())
		{
			paper::account representative;
			paper::private_key prv;
			std::shared_ptr <paper::send_block> block;
			{
				hash.clear ();
				paper::transaction transaction (wallet->node.store.environment, nullptr, false);
				representative = wallet->store.representative (transaction);
				for (auto i (wallet->node.store.pending_begin (transaction, hash)), n (wallet->node.store.pending_end ()); i != n; ++i)
				{
					paper::receivable receivable (i->second);
					if (receivable.source == account_a)
					{
						hash = i->first;
						auto block_l (wallet->node.store.block_get (transaction, i->first));
						assert (dynamic_cast <paper::send_block *> (block_l.get ()) != nullptr);
						block.reset (static_cast <paper::send_block *> (block_l.release ()));
						auto error (wallet->store.fetch (transaction, receivable.destination, prv));
						if (error)
						{
							BOOST_LOG (wallet->node.log) << boost::str (boost::format ("Unable to fetch key for: %1%, stopping pending search") % receivable.destination.to_base58check ());
							block.reset ();
						}
					}
				}
			}
			if (block != nullptr)
			{
				auto wallet_l (wallet);
				wallet->node.wallets.queue_wallet_action (block->hashables.destination, [wallet_l, block, representative, prv] ()
				{
					BOOST_LOG (wallet_l->node.log) << boost::str (boost::format ("Receiving block: %1%") % block->hash ().to_string ());
					auto error (wallet_l->receive_action (*block, prv, representative));
					if (error)
					{
						BOOST_LOG (wallet_l->node.log) << boost::str (boost::format ("Error receiving block %1%") % block->hash ().to_string ());
					}
				});
			}
			prv.clear ();
		}
		if (!current_block.is_zero ())
		{
			run ();
		}
	}
	paper::block_hash current_block;
	std::unordered_set <paper::uint256_union> keys;
	std::shared_ptr <paper::wallet> wallet;
};
}

bool paper::wallet::search_pending ()
{
	paper::transaction transaction (store.environment, nullptr, false);
	auto result (!store.valid_password (transaction));
	if (!result)
	{
		auto search (std::make_shared <search_action> (shared_from_this (), transaction));
		node.service.add (std::chrono::system_clock::now (), [search] ()
		{
			search->run ();
		});
	}
	else
	{
		BOOST_LOG (node.log) << "Stopping search, wallet is locked";
	}
	return result;
}

void paper::wallet::work_generate (paper::account const & account_a, paper::block_hash const & root_a)
{
	auto begin (std::chrono::system_clock::now ());
	if (node.config.logging.work_generation_time ())
	{
		BOOST_LOG (node.log) << "Beginning work generation";
	}
    auto work (node.work.generate (root_a));
	if (node.config.logging.work_generation_time ())
	{
		BOOST_LOG (node.log) << "Work generation complete: " << (std::chrono::duration_cast <std::chrono::microseconds> (std::chrono::system_clock::now () - begin).count ()) << "us";
	}
	paper::transaction transaction (store.environment, nullptr, true);
    work_update (transaction, account_a, root_a, work);
}

paper::wallets::wallets (bool & error_a, paper::node & node_a) :
node (node_a)
{
	if (!error_a)
	{
		paper::transaction transaction (node.store.environment, nullptr, true);
		auto status (mdb_dbi_open (transaction, nullptr, MDB_CREATE, &handle));
		assert (status == 0);
		std::string beginning (paper::uint256_union (0).to_string ());
		std::string end ((paper::uint256_union (paper::uint256_t (0) - paper::uint256_t (1))).to_string ());
		for (paper::store_iterator i (transaction, handle, paper::mdb_val (beginning.size (), const_cast <char *> (beginning.c_str ()))), n (transaction, handle, paper::mdb_val (end.size (), const_cast <char *> (end.c_str ()))); i != n; ++i)
		{
			paper::uint256_union id;
			std::string text (reinterpret_cast <char const *> (i->first.mv_data), i->first.mv_size);
			auto error (id.decode_hex (text));
			assert (!error);
			assert (items.find (id) == items.end ());
			auto wallet (std::make_shared <paper::wallet> (error, transaction, node_a, text));
			if (!error)
			{
				node_a.service.add (std::chrono::system_clock::now (), [wallet] ()
				{
					paper::transaction transaction (wallet->store.environment, nullptr, true);
					wallet->enter_initial_password (transaction);
				});
				items [id] = wallet;
			}
			else
			{
				// Couldn't open wallet
			}
		}
	}
}

std::shared_ptr <paper::wallet> paper::wallets::open (paper::uint256_union const & id_a)
{
    std::shared_ptr <paper::wallet> result;
    auto existing (items.find (id_a));
    if (existing != items.end ())
    {
        result = existing->second;
    }
    return result;
}

std::shared_ptr <paper::wallet> paper::wallets::create (paper::uint256_union const & id_a)
{
    assert (items.find (id_a) == items.end ());
    std::shared_ptr <paper::wallet> result;
    bool error;
	paper::transaction transaction (node.store.environment, nullptr, true);
    auto wallet (std::make_shared <paper::wallet> (error, transaction, node, id_a.to_string ()));
    if (!error)
    {
		node.service.add (std::chrono::system_clock::now (), [wallet] ()
		{
			paper::transaction transaction (wallet->store.environment, nullptr, true);
			wallet->enter_initial_password (transaction);
		});
        items [id_a] = wallet;
        result = wallet;
    }
    return result;
}

bool paper::wallets::search_pending (paper::uint256_union const & wallet_a)
{
	auto result (false);
	auto existing (items.find (wallet_a));
	result = existing == items.end ();
	if (!result)
	{
		auto wallet (existing->second);
		result = wallet->search_pending ();
	}
	return result;
}

void paper::wallets::destroy (paper::uint256_union const & id_a)
{
	paper::transaction transaction (node.store.environment, nullptr, true);
	auto existing (items.find (id_a));
	assert (existing != items.end ());
	auto wallet (existing->second);
	items.erase (existing);
	wallet->store.destroy (transaction);
}

void paper::wallets::queue_wallet_action (paper::account const & account_a, std::function <void ()> const & action_a)
{
	auto current (std::move (action_a));
	auto perform (false);
	{
		std::lock_guard <std::mutex> lock (action_mutex);
		perform = current_actions.insert (account_a).second;
		if (!perform)
		{
			pending_actions.insert (decltype (pending_actions)::value_type (account_a, std::move (current)));
		}
	}
	while (perform)
	{
		current ();
		std::lock_guard <std::mutex> lock (node.wallets.action_mutex);
		auto existing (node.wallets.pending_actions.find (account_a));
		if (existing != node.wallets.pending_actions.end ())
		{
			current = std::move (existing->second);
			node.wallets.pending_actions.erase (existing);
		}
		else
		{
			auto erased (node.wallets.current_actions.erase (account_a));
			assert (erased == 1); (void) erased;
			perform = false;
		}
	}
}

void paper::wallets::foreach_representative (std::function <void (paper::public_key const & pub_a, paper::private_key const & prv_a)> const & action_a)
{
    for (auto i (items.begin ()), n (items.end ()); i != n; ++i)
    {
		paper::transaction transaction (node.store.environment, nullptr, false);
        auto & wallet (*i->second);
		for (auto j (wallet.store.begin (transaction)), m (wallet.store.end ()); j != m; ++j)
        {
			if (wallet.store.valid_password (transaction))
			{
				if (!node.ledger.weight (transaction, j->first).is_zero ())
				{
					paper::private_key prv;
					auto error (wallet.store.fetch (transaction, j->first, prv));
					assert (!error);
					action_a (j->first, prv);
					prv.clear ();
				}
			}
			else
			{
				BOOST_LOG (node.log) << boost::str (boost::format ("Skipping locked wallet %1%") % i->first.to_string ());;
			}
        }
    }
}

paper::store_iterator paper::wallet_store::begin (MDB_txn * transaction_a)
{
    paper::store_iterator result (transaction_a, handle, paper::uint256_union (special_count).val ());
    return result;
}

paper::store_iterator paper::wallet_store::find (MDB_txn * transaction_a, paper::uint256_union const & key)
{
    paper::store_iterator result (transaction_a, handle, key.val ());
    paper::store_iterator end (nullptr);
    if (result != end)
    {
        if (paper::uint256_union (result->first) == key)
        {
            return result;
        }
        else
        {
            return end;
        }
    }
    else
    {
        return end;
    }
}

paper::store_iterator paper::wallet_store::end ()
{
    return paper::store_iterator (nullptr);
}

void paper::processor_service::run ()
{
    std::unique_lock <std::mutex> lock (mutex);
    while (!done)
    {
        if (!operations.empty ())
        {
            auto & operation_l (operations.top ());
            if (operation_l.wakeup < std::chrono::system_clock::now ())
            {
                auto operation (operation_l);
                operations.pop ();
                lock.unlock ();
                operation.function ();
                lock.lock ();
            }
            else
            {
				auto wakeup (operation_l.wakeup);
                condition.wait_until (lock, wakeup);
            }
        }
        else
        {
            condition.wait (lock);
        }
    }
}

size_t paper::processor_service::poll_one ()
{
    std::unique_lock <std::mutex> lock (mutex);
    size_t result (0);
    if (!operations.empty ())
    {
        auto & operation_l (operations.top ());
        if (operation_l.wakeup < std::chrono::system_clock::now ())
        {
            auto operation (operation_l);
            operations.pop ();
            lock.unlock ();
            operation.function ();
            result = 1;
        }
    }
    return result;
}

size_t paper::processor_service::poll ()
{
    std::unique_lock <std::mutex> lock (mutex);
    size_t result (0);
    auto done_l (false);
    while (!done_l)
    {
        if (!operations.empty ())
        {
            auto & operation_l (operations.top ());
            if (operation_l.wakeup < std::chrono::system_clock::now ())
            {
                auto operation (operation_l);
                operations.pop ();
                lock.unlock ();
                operation.function ();
                ++result;
                lock.lock ();
            }
            else
            {
                done_l = true;
            }
        }
        else
        {
            done_l = true;
        }
    }
    return result;
}

void paper::processor_service::add (std::chrono::system_clock::time_point const & wakeup_a, std::function <void ()> const & operation)
{
    std::lock_guard <std::mutex> lock (mutex);
    if (!done)
    {
        operations.push (paper::operation ({wakeup_a, operation}));
        condition.notify_all ();
    }
}

paper::processor_service::processor_service () :
done (false)
{
}

void paper::processor_service::stop ()
{
    std::lock_guard <std::mutex> lock (mutex);
    done = true;
    while (!operations.empty ())
    {
        operations.pop ();
    }
    condition.notify_all ();
}

bool paper::operation::operator > (paper::operation const & other_a) const
{
    return wakeup > other_a.wakeup;
}

namespace
{
class xorshift1024star
{
public:
    xorshift1024star ():
    p (0)
    {
    }
    std::array <uint64_t, 16> s;
    unsigned p;
    uint64_t next ()
    {
        auto p_l (p);
        auto pn ((p_l + 1) & 15);
        p = pn;
        uint64_t s0 = s[ p_l ];
        uint64_t s1 = s[ pn ];
        s1 ^= s1 << 31; // a
        s1 ^= s1 >> 11; // b
        s0 ^= s0 >> 30; // c
        return ( s[ pn ] = s0 ^ s1 ) * 1181783497276652981LL;
    }
};
}

paper::work_pool::work_pool () :
current (0),
ticket (0),
done (false)
{
	static_assert (ATOMIC_INT_LOCK_FREE == 2, "Atomic int needed");
	auto count (std::max (1u, std::thread::hardware_concurrency ()));
	for (auto i (0); i < count; ++i)
	{
		threads.push_back (std::thread ([this, i] ()
		{
			loop (i);
		}));
	}
}

paper::work_pool::~work_pool ()
{
	stop ();
	for (auto &i: threads)
	{
		i.join ();
	}
}

void paper::work_pool::loop (uint64_t thread)
{
    xorshift1024star rng;
    rng.s.fill (0x0123456789abcdef + thread);// No seed here, we're not securing anything, s just can't be 0 per the xorshift1024star spec
	uint64_t work;
	uint64_t output;
    blake2b_state hash;
	blake2b_init (&hash, sizeof (output));
	std::unique_lock <std::mutex> lock (mutex);
	while (!done || !pending.empty())
	{
		auto current_l (current);
		if (!current_l.is_zero ())
		{
			int ticket_l (ticket);
			lock.unlock ();
			output = 0;
			while (ticket == ticket_l && output < paper::block::publish_threshold)
			{
				auto iteration (std::numeric_limits <uint16_t>::max ());
				while (iteration && output < paper::block::publish_threshold)
				{
					work = rng.next ();
					blake2b_update (&hash, reinterpret_cast <uint8_t *> (&work), sizeof (work));
					blake2b_update (&hash, current_l.bytes.data (), current_l.bytes.size ());
					blake2b_final (&hash, reinterpret_cast <uint8_t *> (&output), sizeof (output));
					blake2b_init (&hash, sizeof (output));
					iteration -= 1;
				}
			}
			lock.lock ();
			if (current == current_l)
			{
				assert (output >= paper::block::publish_threshold);
				++ticket;
				completed [current_l] = work;
				consumer_condition.notify_all ();
				// Change current so only one work thread publishes their result
				current.clear ();
			}
		}
		else
		{
			if (!pending.empty ())
			{
				current = pending.front ();
				pending.pop ();
				producer_condition.notify_all ();
			}
			else
			{
				producer_condition.wait (lock);
			}
		}
	}
}

void paper::work_pool::generate (paper::block & block_a)
{
    block_a.block_work_set (generate (block_a.root ()));
}

void paper::work_pool::stop ()
{
	std::lock_guard <std::mutex> lock (mutex);
	done = true;
	producer_condition.notify_all ();
}

uint64_t paper::work_pool::generate (paper::uint256_union const & root_a)
{
	assert (!root_a.is_zero ());
	uint64_t result;
	std::unique_lock <std::mutex> lock (mutex);
	pending.push (root_a);
	producer_condition.notify_one ();
	auto done (false);
	while (!done)
	{
		consumer_condition.wait (lock);
		auto finish (completed.find (root_a));
		if (finish != completed.end ())
		{
			done = true;
			result = finish->second;
			completed.erase (finish);
		}
	}
	return result;
}

namespace
{
    size_t constexpr stepping (16);
}
paper::kdf::kdf (size_t entries_a) :
entries (entries_a),
data (new uint64_t [entries_a])
{
    assert ((entries_a & (stepping - 1)) == 0);
}

// Derive a wallet key from a password and salt.
paper::uint256_union paper::kdf::generate (std::string const & password_a, paper::uint256_union const & salt_a)
{
    paper::uint256_union input;
    blake2b_state hash;
	blake2b_init (&hash, 32);
    blake2b_update (&hash, reinterpret_cast <uint8_t const *> (password_a.data ()), password_a.size ());
    blake2b_final (&hash, input.bytes.data (), input.bytes.size ());
    input ^= salt_a;
    blake2b_init (&hash, 32);
    auto entries_l (entries);
    auto mask (entries_l - 1);
    xorshift1024star rng;
    rng.s [0] = input.qwords [0];
    rng.s [1] = input.qwords [1];
    rng.s [2] = input.qwords [2];
    rng.s [3] = input.qwords [3];
    for (auto i (4), n (16); i != n; ++i)
    {
        rng.s [i] = 0;
    }
    // Random-fill buffer for an initialized starting point
    for (auto i (data.get ()), n (data.get () + entries_l); i != n; ++i)
    {
        auto next (rng.next ());
        *i = next;
    }
    auto previous (rng.next ());
    // Random-write buffer to break n+1 = f(n) relation
    for (size_t i (0), n (entries); i != n; ++i)
    {
        auto index (previous & mask);
        auto value (rng.next ());
		// Use the index from the previous random value so LSB (data[index]) != value
        data [index] = value;
    }
    // Random-read buffer to prevent partial memorization
    union
    {
        std::array <uint64_t, stepping> qwords;
        std::array <uint8_t, stepping * sizeof (uint64_t)> bytes;
    } value;
	// Hash the memory buffer to derive encryption key
    for (size_t i (0), n (entries); i != n; i += stepping)
    {
        for (size_t j (0), m (stepping); j != m; ++j)
        {
            auto index (rng.next () % (entries_l - (i + j)));
            value.qwords [j] = data [index];
            data [index] = data [entries_l - (i + j) - 1];
        }
        blake2b_update (&hash, reinterpret_cast <uint8_t *> (value.bytes.data ()), stepping * sizeof (uint64_t));
    }
    paper::uint256_union result;
    blake2b_final (&hash, result.bytes.data (), result.bytes.size ());
    return result;
}

paper::logging::logging () :
ledger_logging_value (false),
ledger_duplicate_logging_value (false),
network_logging_value (true),
network_message_logging_value (false),
network_publish_logging_value (false),
network_packet_logging_value (false),
network_keepalive_logging_value (false),
node_lifetime_tracing_value (false),
insufficient_work_logging_value (true),
log_rpc_value (true),
bulk_pull_logging_value (false),
work_generation_time_value (true),
log_to_cerr_value (false)
{
}

void paper::logging::serialize_json (boost::property_tree::ptree & tree_a) const
{
	tree_a.put ("ledger", ledger_logging_value);
	tree_a.put ("ledger_duplicate", ledger_duplicate_logging_value);
	tree_a.put ("network", network_logging_value);
	tree_a.put ("network_message", network_message_logging_value);
	tree_a.put ("network_publish", network_publish_logging_value);
	tree_a.put ("network_packet", network_packet_logging_value);
	tree_a.put ("network_keepalive", network_keepalive_logging_value);
	tree_a.put ("node_lifetime_tracing", node_lifetime_tracing_value);
	tree_a.put ("insufficient_work", insufficient_work_logging_value);
	tree_a.put ("log_rpc", log_rpc_value);
	tree_a.put ("bulk_pull", bulk_pull_logging_value);
	tree_a.put ("work_generation_time", work_generation_time_value);
	tree_a.put ("log_to_cerr", log_to_cerr_value);
}

bool paper::logging::deserialize_json (boost::property_tree::ptree const & tree_a)
{
	auto result (false);
	try
	{
		ledger_logging_value = tree_a.get <bool> ("ledger");
		ledger_duplicate_logging_value = tree_a.get <bool> ("ledger_duplicate");
		network_logging_value = tree_a.get <bool> ("network");
		network_message_logging_value = tree_a.get <bool> ("network_message");
		network_publish_logging_value = tree_a.get <bool> ("network_publish");
		network_packet_logging_value = tree_a.get <bool> ("network_packet");
		network_keepalive_logging_value = tree_a.get <bool> ("network_keepalive");
		node_lifetime_tracing_value = tree_a.get <bool> ("node_lifetime_tracing");
		insufficient_work_logging_value = tree_a.get <bool> ("insufficient_work");
		log_rpc_value = tree_a.get <bool> ("log_rpc");
		bulk_pull_logging_value = tree_a.get <bool> ("bulk_pull");
		work_generation_time_value = tree_a.get <bool> ("work_generation_time");
		log_to_cerr_value = tree_a.get <bool> ("log_to_cerr");
	}
	catch (std::runtime_error const &)
	{
		result = true;
	}
	return result;
}

bool paper::logging::ledger_logging () const
{
	return ledger_logging_value;
}

bool paper::logging::ledger_duplicate_logging () const
{
	return ledger_logging () && ledger_duplicate_logging_value;
}
bool paper::logging::network_logging () const
{
	return network_logging_value;
}

bool paper::logging::network_message_logging () const
{
	return network_logging () && network_message_logging_value;
}

bool paper::logging::network_publish_logging () const
{
	return network_logging () && network_publish_logging_value;
}

bool paper::logging::network_packet_logging () const
{
	return network_logging () && network_packet_logging_value;
}

bool paper::logging::network_keepalive_logging () const
{
	return network_logging () && network_keepalive_logging_value;
}

bool paper::logging::node_lifetime_tracing () const
{
	return node_lifetime_tracing_value;
}

bool paper::logging::insufficient_work_logging () const
{
	return network_logging () && insufficient_work_logging_value;
}

bool paper::logging::log_rpc () const
{
	return network_logging () && log_rpc_value;
}

bool paper::logging::bulk_pull_logging () const
{
	return network_logging () && bulk_pull_logging_value;
}

bool paper::logging::work_generation_time () const
{
	return work_generation_time_value;
}

bool paper::logging::log_to_cerr () const
{
	return log_to_cerr_value;
}

paper::node_init::node_init () :
block_store_init (false),
wallet_init (false)
{
}

bool paper::node_init::error ()
{
    return block_store_init || wallet_init;
}

namespace {
class send_visitor : public paper::block_visitor
{
public:
	send_visitor (paper::node & node_a) :
	node (node_a)
	{
	}
	void send_block (paper::send_block const & block_a)
	{
		auto receive (false);
		{
			paper::transaction transaction (node.store.environment, nullptr, false);
			for (auto i (node.wallets.items.begin ()), n (node.wallets.items.end ()); i != n && receive == false; ++i)
			{
				auto & wallet (*i->second);
				if (wallet.store.find (transaction, block_a.hashables.destination) != wallet.store.end ())
				{
					receive = true;
				}
			}
		}
		if (receive)
		{
			if (node.config.logging.ledger_logging ())
			{
				BOOST_LOG (node.log) << boost::str (boost::format ("Starting fast confirmation of block: %1%") % block_a.hash ().to_string ());
			}
			auto node_l (node.shared ());
			node.conflicts.start (block_a, [node_l] (paper::block & block_a)
			{
				node_l->process_confirmed (block_a);
			}, false);
			auto root (block_a.root ());
			std::shared_ptr <paper::block> block_l (block_a.clone ().release ());
			node.service.add (std::chrono::system_clock::now () + paper::confirm_wait, [node_l, root, block_l] ()
			{
				if (node_l->conflicts.no_conflict (root))
				{
					node_l->process_confirmed (*block_l);
				}
				else
				{
					if (node_l->config.logging.ledger_logging ())
					{
						BOOST_LOG (node_l->log) << boost::str (boost::format ("Unable to fast-confirm block: %1% because root: %2% is in conflict") % block_l->hash ().to_string () % root.to_string ());
					}
				}
			});
        }
	}
	void receive_block (paper::receive_block const &)
	{
	}
	void open_block (paper::open_block const &)
	{
	}
	void change_block (paper::change_block const &)
	{
	}
	paper::node & node;
};
}

paper::node_config::node_config () :
peering_port (paper::network::node_port),
packet_delay_microseconds (5000),
bootstrap_fraction_numerator (1),
creation_rebroadcast (2),
rebroadcast_delay (15)
{
	preconfigured_peers.push_back ("paper.paperblocks.net");
}

paper::node_config::node_config (uint16_t peering_port_a, paper::logging const & logging_a) :
peering_port (peering_port_a),
logging (logging_a),
packet_delay_microseconds (5000),
bootstrap_fraction_numerator (1),
creation_rebroadcast (2),
rebroadcast_delay (15)
{
}

void paper::node_config::serialize_json (boost::property_tree::ptree & tree_a) const
{
	tree_a.put ("peering_port", std::to_string (peering_port));
	tree_a.put ("packet_delay_microseconds", std::to_string (packet_delay_microseconds));
	tree_a.put ("bootstrap_fraction_numerator", std::to_string (bootstrap_fraction_numerator));
	tree_a.put ("creation_rebroadcast", std::to_string (creation_rebroadcast));
	tree_a.put ("rebroadcast_delay", std::to_string (rebroadcast_delay));
	boost::property_tree::ptree logging_l;
	logging.serialize_json (logging_l);
	tree_a.add_child ("logging", logging_l);
	boost::property_tree::ptree preconfigured_peers_l;
	for (auto i (preconfigured_peers.begin ()), n (preconfigured_peers.end ()); i != n; ++i)
	{
		boost::property_tree::ptree entry;
		entry.put ("", *i);
		preconfigured_peers_l.push_back (std::make_pair ("", entry));
	}
	tree_a.add_child ("preconfigured_peers", preconfigured_peers_l);
}

bool paper::node_config::deserialize_json (boost::property_tree::ptree const & tree_a)
{
	auto result (false);
	try
	{
		auto peering_port_l (tree_a.get <std::string> ("peering_port"));
		auto packet_delay_microseconds_l (tree_a.get <std::string> ("packet_delay_microseconds"));
		auto bootstrap_fraction_numerator_l (tree_a.get <std::string> ("bootstrap_fraction_numerator"));
		auto creation_rebroadcast_l (tree_a.get <std::string> ("creation_rebroadcast"));
		auto rebroadcast_delay_l (tree_a.get <std::string> ("rebroadcast_delay"));
		auto logging_l (tree_a.get_child ("logging"));
		auto preconfigured_peers_l (tree_a.get_child ("preconfigured_peers"));
		preconfigured_peers.clear ();
		for (auto i (preconfigured_peers_l.begin ()), n (preconfigured_peers_l.end ()); i != n; ++i)
		{
			auto bootstrap_peer (i->second.get <std::string> (""));
			preconfigured_peers.push_back (bootstrap_peer);
		}
		try
		{
			peering_port = std::stoul (peering_port_l);
			packet_delay_microseconds = std::stoul (packet_delay_microseconds_l);
			bootstrap_fraction_numerator = std::stoul (bootstrap_fraction_numerator_l);
			creation_rebroadcast = std::stoul (creation_rebroadcast_l);
			rebroadcast_delay = std::stoul (rebroadcast_delay_l);
			result = result || creation_rebroadcast > 10;
			result = result || rebroadcast_delay > 300;
			result = result || peering_port > std::numeric_limits <uint16_t>::max ();
			result = result || logging.deserialize_json (logging_l);
		}
		catch (std::logic_error const &)
		{
			result = true;
		}
	}
	catch (std::runtime_error const &)
	{
		result = true;
	}
	return result;
}

paper::node::node (paper::node_init & init_a, boost::shared_ptr <boost::asio::io_service> service_a, uint16_t peering_port_a, boost::filesystem::path const & application_path_a, paper::processor_service & processor_a, paper::logging const & logging_a, paper::work_pool & work_a) :
node (init_a, service_a, application_path_a, processor_a, paper::node_config (peering_port_a, logging_a), work_a)
{
}

paper::node::node (paper::node_init & init_a, boost::shared_ptr <boost::asio::io_service> service_a, boost::filesystem::path const & application_path_a, paper::processor_service & processor_a, paper::node_config const & config_a, paper::work_pool & work_a) :
config (config_a),
service (processor_a),
work (work_a),
store (init_a.block_store_init, application_path_a / "data.ldb"),
gap_cache (*this),
ledger (store),
conflicts (*this),
wallets (init_a.block_store_init, *this),
network (*service_a, config.peering_port, *this),
bootstrap_initiator (*this),
bootstrap (*service_a, config.peering_port, *this),
peers (network.endpoint ()),
application_path (application_path_a)
{
	peers.peer_observer = [this] (paper::endpoint const & endpoint_a)
	{
		for (auto i: endpoint_observers)
		{
			i (endpoint_a);
		}
	};
	peers.disconnect_observer = [this] ()
	{
		for (auto i: disconnect_observers)
		{
			i ();
		}
	};
	endpoint_observers.push_back ([this] (paper::endpoint const & endpoint_a)
	{
		network.send_keepalive (endpoint_a);
		bootstrap_initiator.warmup (endpoint_a);
	});
    vote_observers.push_back ([this] (paper::vote const & vote_a)
    {
        conflicts.update (vote_a);
    });
    vote_observers.push_back ([this] (paper::vote const & vote_a)
    {
		paper::transaction transaction (store.environment, nullptr, false);
        gap_cache.vote (transaction, vote_a);
    });
    if (config.logging.log_to_cerr ())
    {
        boost::log::add_console_log (std::cerr, boost::log::keywords::format = "[%TimeStamp%]: %Message%");
    }
    boost::log::add_common_attributes ();
    boost::log::add_file_log (boost::log::keywords::target = application_path_a / "log", boost::log::keywords::file_name = application_path_a / "log" / "log_%Y-%m-%d_%H-%M-%S.%N.log", boost::log::keywords::rotation_size = 4 * 1024 * 1024, boost::log::keywords::auto_flush = true, boost::log::keywords::scan_method = boost::log::sinks::file::scan_method::scan_matching, boost::log::keywords::max_size = 16 * 1024 * 1024, boost::log::keywords::format = "[%TimeStamp%]: %Message%");
    BOOST_LOG (log) << "Node starting, version: " << RAIBLOCKS_VERSION_MAJOR << "." << RAIBLOCKS_VERSION_MINOR << "." << RAIBLOCKS_VERSION_PATCH;
	observers.push_back ([this] (paper::block const & block_a, paper::account const & account_a)
    {
		send_visitor visitor (*this);
		block_a.visit (visitor);
    });
    if (!init_a.error ())
    {
        if (config.logging.node_lifetime_tracing ())
        {
            std::cerr << "Constructing node\n";
        }
		paper::transaction transaction (store.environment, nullptr, true);
        if (store.latest_begin (transaction) == store.latest_end ())
        {
            // Store was empty meaning we just created it, add the genesis block
            paper::genesis genesis;
            genesis.initialize (transaction, store);
        }
    }
}

paper::node::~node ()
{
    if (config.logging.node_lifetime_tracing ())
    {
        std::cerr << "Destructing node\n";
    }
}

void paper::node::send_keepalive (paper::endpoint const & endpoint_a)
{
    auto endpoint_l (endpoint_a);
    if (endpoint_l.address ().is_v4 ())
    {
        endpoint_l = paper::endpoint (boost::asio::ip::address_v6::v4_mapped (endpoint_l.address ().to_v4 ()), endpoint_l.port ());
    }
    assert (endpoint_l.address ().is_v6 ());
    network.send_keepalive (endpoint_l);
}

void paper::node::vote (paper::vote const & vote_a)
{
    for (auto & i: vote_observers)
    {
        i (vote_a);
    }
}

paper::gap_cache::gap_cache (paper::node & node_a) :
node (node_a)
{
}

void paper::gap_cache::add (paper::block const & block_a, paper::block_hash needed_a)
{
    std::lock_guard <std::mutex> lock (mutex);
    auto existing (blocks.find (needed_a));
    if (existing != blocks.end ())
    {
        blocks.modify (existing, [] (paper::gap_information & info) {info.arrival = std::chrono::system_clock::now ();});
    }
    else
    {
        auto hash (block_a.hash ());
		blocks.insert ({std::chrono::system_clock::now (), needed_a, hash, std::unique_ptr <paper::votes> (new paper::votes (hash)), block_a.clone ()});
        if (blocks.size () > max)
        {
            blocks.get <1> ().erase (blocks.get <1> ().begin ());
        }
    }
}

std::unique_ptr <paper::block> paper::gap_cache::get (paper::block_hash const & hash_a)
{
    std::lock_guard <std::mutex> lock (mutex);
    std::unique_ptr <paper::block> result;
    auto existing (blocks.find (hash_a));
    if (existing != blocks.end ())
    {
        blocks.modify (existing, [&] (paper::gap_information & info) {result.swap (info.block);});
        blocks.erase (existing);
    }
    return result;
}

void paper::gap_cache::vote (MDB_txn * transaction_a, paper::vote const & vote_a)
{
    std::lock_guard <std::mutex> lock (mutex);
    auto hash (vote_a.block->hash ());
    auto existing (blocks.get <2> ().find (hash));
    if (existing != blocks.get <2> ().end ())
    {
        auto changed (existing->votes->vote (vote_a));
        if (changed)
        {
            auto winner (node.ledger.winner (transaction_a, *existing->votes));
            if (winner.first > bootstrap_threshold (transaction_a))
            {
                BOOST_LOG (node.log) << boost::str (boost::format ("Initiating bootstrap for confirmed gap: %1%") % hash.to_string ());
                node.bootstrap_initiator.bootstrap_any ();
            }
        }
    }
}

paper::uint128_t paper::gap_cache::bootstrap_threshold (MDB_txn * transaction_a)
{
    auto result ((node.ledger.supply (transaction_a) / 256) * node.config.bootstrap_fraction_numerator);
	return result;
}

bool paper::network::confirm_broadcast (std::vector <paper::peer_information> & list_a, std::unique_ptr <paper::block> block_a, uint64_t sequence_a, size_t rebroadcast_a)
{
    bool result (false);
	node.wallets.foreach_representative ([&result, &block_a, &list_a, this, sequence_a, rebroadcast_a] (paper::public_key const & pub_a, paper::private_key const & prv_a)
	{
		auto hash (block_a->hash ());
		for (auto j (list_a.begin ()), m (list_a.end ()); j != m; ++j)
		{
			if (!node.peers.knows_about (j->endpoint, hash))
			{
				confirm_block (prv_a, pub_a, block_a->clone (), sequence_a, j->endpoint, rebroadcast_a);
				result = true;
			}
		}
	});
    return result;
}

void paper::network::confirm_block (paper::private_key const & prv, paper::public_key const & pub, std::unique_ptr <paper::block> block_a, uint64_t sequence_a, paper::endpoint const & endpoint_a, size_t rebroadcast_a)
{
    paper::confirm_ack confirm (pub, prv, sequence_a, std::move (block_a));
    std::shared_ptr <std::vector <uint8_t>> bytes (new std::vector <uint8_t>);
    {
        paper::vectorstream stream (*bytes);
        confirm.serialize (stream);
    }
    if (node.config.logging.network_publish_logging ())
    {
        BOOST_LOG (node.log) << boost::str (boost::format ("Sending confirm_ack for block %1% to %2%") % confirm.vote.block->hash ().to_string () % endpoint_a);
    }
    auto node_l (node.shared ());
    node.network.send_buffer (bytes->data (), bytes->size (), endpoint_a, 0, [bytes, node_l, endpoint_a] (boost::system::error_code const & ec, size_t size_a)
        {
            if (node_l->config.logging.network_logging ())
            {
                if (ec)
                {
                    BOOST_LOG (node_l->log) << boost::str (boost::format ("Error broadcasting confirm_ack to %1%: %2%") % endpoint_a % ec.message ());
                }
            }
        });
}

void paper::node::process_receive_republish (std::unique_ptr <paper::block> incoming, size_t rebroadcast_a)
{
	assert (incoming != nullptr);
    std::unique_ptr <paper::block> block (std::move (incoming));
    do
    {
        auto hash (block->hash ());
        auto process_result (process_receive (*block));
        switch (process_result.code)
        {
            case paper::process_result::progress:
            {
                network.republish_block (std::move (block), rebroadcast_a);
                break;
            }
            default:
            {
                break;
            }
        }
        block = gap_cache.get (hash);
    }
    while (block != nullptr);
}

paper::process_return paper::node::process_receive (paper::block const & block_a)
{
	paper::process_return result;
	{
		paper::transaction transaction (store.environment, nullptr, true);
		result = ledger.process (transaction, block_a);
	}
    switch (result.code)
    {
        case paper::process_result::progress:
        {
			call_observers (block_a, result.account);
            if (config.logging.ledger_logging ())
            {
                std::string block;
                block_a.serialize_json (block);
                BOOST_LOG (log) << boost::str (boost::format ("Processing block %1% %2%") % block_a.hash ().to_string () % block);
            }
            break;
        }
        case paper::process_result::gap_previous:
        {
            if (config.logging.ledger_logging ())
            {
                BOOST_LOG (log) << boost::str (boost::format ("Gap previous for: %1%") % block_a.hash ().to_string ());
            }
            auto previous (block_a.previous ());
            gap_cache.add (block_a, previous);
            break;
        }
        case paper::process_result::gap_source:
        {
            if (config.logging.ledger_logging ())
            {
                BOOST_LOG (log) << boost::str (boost::format ("Gap source for: %1%") % block_a.hash ().to_string ());
            }
            auto source (block_a.source ());
            gap_cache.add (block_a, source);
            break;
        }
        case paper::process_result::old:
        {
            if (config.logging.ledger_duplicate_logging ())
            {
                BOOST_LOG (log) << boost::str (boost::format ("Old for: %1%") % block_a.hash ().to_string ());
            }
            break;
        }
        case paper::process_result::bad_signature:
        {
            if (config.logging.ledger_logging ())
            {
                BOOST_LOG (log) << boost::str (boost::format ("Bad signature for: %1%") % block_a.hash ().to_string ());
            }
            break;
        }
        case paper::process_result::overspend:
        {
            if (config.logging.ledger_logging ())
            {
                BOOST_LOG (log) << boost::str (boost::format ("Overspend for: %1%") % block_a.hash ().to_string ());
            }
            break;
        }
        case paper::process_result::unreceivable:
        {
            if (config.logging.ledger_logging ())
            {
                BOOST_LOG (log) << boost::str (boost::format ("Unreceivable for: %1%") % block_a.hash ().to_string ());
            }
            break;
        }
        case paper::process_result::not_receive_from_send:
        {
            if (config.logging.ledger_logging ())
            {
                BOOST_LOG (log) << boost::str (boost::format ("Not receive from spend for: %1%") % block_a.hash ().to_string ());
            }
            break;
        }
        case paper::process_result::fork:
        {
            if (config.logging.ledger_logging ())
            {
                BOOST_LOG (log) << boost::str (boost::format ("Fork for: %1%") % block_a.hash ().to_string ());
            }
			std::unique_ptr <paper::block> root;
			{
				paper::transaction transaction (store.environment, nullptr, false);
				root = ledger.successor (transaction, block_a.root ());
			}
			auto node_l (shared_from_this ());
			conflicts.start (*root, [node_l] (paper::block & block_a)
			{
				node_l->process_confirmed (block_a);
			}, false);
            break;
        }
        case paper::process_result::account_mismatch:
        {
            if (config.logging.ledger_logging ())
            {
                BOOST_LOG (log) << boost::str (boost::format ("Account mismatch for: %1%") % block_a.hash ().to_string ());
            }
        }
    }
    return result;
}

paper::process_return paper::node::process (paper::block const & block_a)
{
	paper::transaction transaction (store.environment, nullptr, true);
	auto result (ledger.process (transaction, block_a));
	return result;
}

std::vector <paper::peer_information> paper::peer_container::list ()
{
    std::vector <paper::peer_information> result;
    std::lock_guard <std::mutex> lock (mutex);
    result.reserve (peers.size ());
    for (auto i (peers.begin ()), j (peers.end ()); i != j; ++i)
    {
        result.push_back (*i);
    }
    return result;
}

std::vector <paper::peer_information> paper::peer_container::bootstrap_candidates ()
{
    std::vector <paper::peer_information> result;
    std::lock_guard <std::mutex> lock (mutex);
	auto now (std::chrono::system_clock::now ());
    for (auto i (peers.begin ()), j (peers.end ()); i != j; ++i)
    {
		if (now - i->last_bootstrap_failure > std::chrono::minutes (15))
		{
			result.push_back (*i);
		}
    }
    return result;
}

void paper::publish::visit (paper::message_visitor & visitor_a) const
{
    visitor_a.publish (*this);
}

paper::keepalive::keepalive () :
message (paper::message_type::keepalive)
{
    boost::asio::ip::udp::endpoint endpoint (boost::asio::ip::address_v6 {}, 0);
    for (auto i (peers.begin ()), n (peers.end ()); i != n; ++i)
    {
        *i = endpoint;
    }
}

void paper::keepalive::visit (paper::message_visitor & visitor_a) const
{
    visitor_a.keepalive (*this);
}

void paper::keepalive::serialize (paper::stream & stream_a)
{
    write_header (stream_a);
    for (auto i (peers.begin ()), j (peers.end ()); i != j; ++i)
    {
        assert (i->address ().is_v6 ());
        auto bytes (i->address ().to_v6 ().to_bytes ());
        write (stream_a, bytes);
        write (stream_a, i->port ());
    }
}

bool paper::keepalive::deserialize (paper::stream & stream_a)
{
	auto result (read_header (stream_a, version_max, version_using, version_min, type, extensions));
	assert (!result);
    assert (type == paper::message_type::keepalive);
    for (auto i (peers.begin ()), j (peers.end ()); i != j; ++i)
    {
        std::array <uint8_t, 16> address;
        uint16_t port;
        read (stream_a, address);
        read (stream_a, port);
        *i = paper::endpoint (boost::asio::ip::address_v6 (address), port);
    }
    return result;
}

size_t paper::processor_service::size ()
{
    std::lock_guard <std::mutex> lock (mutex);
    return operations.size ();
}

paper::system::system (uint16_t port_a, size_t count_a) :
service (new boost::asio::io_service)
{
    nodes.reserve (count_a);
    for (size_t i (0); i < count_a; ++i)
    {
        paper::node_init init;
		paper::node_config config (port_a + i, logging);
        auto node (std::make_shared <paper::node> (init, service, paper::unique_path (), processor, config, work));
        assert (!init.error ());
        node->start ();
		paper::uint256_union wallet;
		paper::random_pool.GenerateBlock (wallet.bytes.data (), wallet.bytes.size ());
		node->wallets.create (wallet);
        nodes.push_back (node);
    }
    for (auto i (nodes.begin ()), j (nodes.begin () + 1), n (nodes.end ()); j != n; ++i, ++j)
    {
        auto starting1 ((*i)->peers.size ());
        auto new1 (starting1);
        auto starting2 ((*j)->peers.size ());
        auto new2 (starting2);
        (*j)->network.send_keepalive ((*i)->network.endpoint ());
        do {
            poll ();
            new1 = (*i)->peers.size ();
            new2 = (*j)->peers.size ();
        } while (new1 == starting1 || new2 == starting2);
    }
	auto iterations1 (0);
	while (std::any_of (nodes.begin (), nodes.end (), [] (std::shared_ptr <paper::node> const & node_a) {return node_a->bootstrap_initiator.in_progress;}))
	{
		poll ();
		++iterations1;
		assert (iterations1 < 1000);
	}
}

paper::system::~system ()
{
    for (auto & i: nodes)
    {
        i->stop ();
    }
}

std::shared_ptr <paper::wallet> paper::system::wallet (size_t index_a)
{
    assert (nodes.size () > index_a);
	auto size (nodes [index_a]->wallets.items.size ());
    assert (size == 1);
    return nodes [index_a]->wallets.items.begin ()->second;
}

paper::account paper::system::account (MDB_txn * transaction_a, size_t index_a)
{
    auto wallet_l (wallet (index_a));
    auto keys (wallet_l->store.begin (transaction_a));
    assert (keys != wallet_l->store.end ());
    auto result (keys->first);
    assert (++keys == wallet_l->store.end ());
    return result;
}

void paper::system::poll ()
{
	auto polled1 (service->poll_one ());
	auto polled2 (processor.poll_one ());
	if (polled1 == 0 && polled2 == 0)
	{
		std::this_thread::sleep_for (std::chrono::milliseconds (50));
	}
}

void paper::node::process_confirmation (paper::block const & block_a, paper::endpoint const & sender)
{
	wallets.foreach_representative ([this, &block_a, &sender] (paper::public_key const & pub_a, paper::private_key const & prv_a)
	{
		if (config.logging.network_message_logging ())
		{
			BOOST_LOG (log) << boost::str (boost::format ("Sending confirm ack to: %1%") % sender);
		}
		network.confirm_block (prv_a, pub_a, block_a.clone (), 0, sender, 0);
	});
}

paper::confirm_ack::confirm_ack (bool & error_a, paper::stream & stream_a) :
message (error_a, stream_a),
vote (error_a, stream_a, block_type ())
{
}

paper::confirm_ack::confirm_ack (paper::account const & account_a, paper::private_key const & prv_a, uint64_t sequence_a, std::unique_ptr <paper::block> block_a) :
message (paper::message_type::confirm_ack),
vote (account_a, prv_a, sequence_a, std::move (block_a))
{
    block_type_set (vote.block->type ());
}

bool paper::confirm_ack::deserialize (paper::stream & stream_a)
{
	auto result (read_header (stream_a, version_max, version_using, version_min, type, extensions));
	assert (!result);
    assert (type == paper::message_type::confirm_ack);
    if (!result)
    {
        result = read (stream_a, vote.account);
        if (!result)
        {
            result = read (stream_a, vote.signature);
            if (!result)
            {
                result = read (stream_a, vote.sequence);
                if (!result)
                {
                    vote.block = paper::deserialize_block (stream_a, block_type ());
                    result = vote.block == nullptr;
                }
            }
        }
    }
    return result;
}

void paper::confirm_ack::serialize (paper::stream & stream_a)
{
    assert (block_type () == paper::block_type::send || block_type () == paper::block_type::receive || block_type () == paper::block_type::open || block_type () == paper::block_type::change);
	write_header (stream_a);
    write (stream_a, vote.account);
    write (stream_a, vote.signature);
    write (stream_a, vote.sequence);
    vote.block->serialize (stream_a);
}

bool paper::confirm_ack::operator == (paper::confirm_ack const & other_a) const
{
    auto result (vote.account == other_a.vote.account && *vote.block == *other_a.vote.block && vote.signature == other_a.vote.signature && vote.sequence == other_a.vote.sequence);
    return result;
}

void paper::confirm_ack::visit (paper::message_visitor & visitor_a) const
{
    visitor_a.confirm_ack (*this);
}

paper::confirm_req::confirm_req () :
message (paper::message_type::confirm_req)
{
}

paper::confirm_req::confirm_req (std::unique_ptr <paper::block> block_a) :
message (paper::message_type::confirm_req),
block (std::move (block_a))
{
    block_type_set (block->type ());
}

bool paper::confirm_req::deserialize (paper::stream & stream_a)
{
	auto result (read_header (stream_a, version_max, version_using, version_min, type, extensions));
	assert (!result);
    assert (type == paper::message_type::confirm_req);
    if (!result)
	{
        block = paper::deserialize_block (stream_a, block_type ());
        result = block == nullptr;
    }
    return result;
}

void paper::confirm_req::visit (paper::message_visitor & visitor_a) const
{
    visitor_a.confirm_req (*this);
}

void paper::confirm_req::serialize (paper::stream & stream_a)
{
    assert (block != nullptr);
	write_header (stream_a);
    block->serialize (stream_a);
}

paper::rpc_config::rpc_config () :
address (boost::asio::ip::address_v6::v4_mapped (boost::asio::ip::address_v4::loopback ())),
port (paper::network::rpc_port),
enable_control (false)
{
}

paper::rpc_config::rpc_config (bool enable_control_a) :
address (boost::asio::ip::address_v6::v4_mapped (boost::asio::ip::address_v4::loopback ())),
port (paper::network::rpc_port),
enable_control (enable_control_a)
{
}

void paper::rpc_config::serialize_json (boost::property_tree::ptree & tree_a) const
{
    tree_a.put ("address", address.to_string ());
    tree_a.put ("port", std::to_string (port));
    tree_a.put ("enable_control", enable_control);
}

bool paper::rpc_config::deserialize_json (boost::property_tree::ptree const & tree_a)
{
	auto result (false);
    try
    {
		auto address_l (tree_a.get <std::string> ("address"));
		auto port_l (tree_a.get <std::string> ("port"));
		enable_control = tree_a.get <bool> ("enable_control");
		try
		{
			port = std::stoul (port_l);
			result = port > std::numeric_limits <uint16_t>::max ();
		}
		catch (std::logic_error const &)
		{
			result = true;
		}
		boost::system::error_code ec;
		address = boost::asio::ip::address_v6::from_string (address_l, ec);
		if (ec)
		{
			result = true;
		}
    }
    catch (std::runtime_error const &)
    {
        result = true;
    }
	return result;
}

paper::rpc::rpc (boost::shared_ptr <boost::asio::io_service> service_a, boost::shared_ptr <boost::network::utils::thread_pool> pool_a, paper::node & node_a, paper::rpc_config const & config_a) :
config (config_a),
server (decltype (server)::options (*this).address (config.address.to_string ()).port (std::to_string (config.port)).io_service (service_a).thread_pool (pool_a)),
node (node_a)
{
}

void paper::rpc::start ()
{
    server.listen ();
}

void paper::rpc::stop ()
{
    server.stop ();
}

namespace
{
void set_response (boost::network::http::server <paper::rpc>::response & response, boost::property_tree::ptree & tree)
{
    std::stringstream ostream;
    boost::property_tree::write_json (ostream, tree);
    response.status = boost::network::http::server <paper::rpc>::response::ok;
    response.headers.push_back (boost::network::http::response_header_narrow {"Content-Type", "application/json"});
    response.content = ostream.str ();
}
bool parse_port (std::string const & string_a, uint16_t & port_a)
{
	bool result;
	size_t converted;
	port_a = std::stoul (string_a, &converted);
	result = converted != string_a.size () || converted > std::numeric_limits <uint16_t>::max ();
	return result;
}
bool parse_address_port (std::string const & string, boost::asio::ip::address & address_a, uint16_t & port_a)
{
    auto result (false);
    auto port_position (string.rfind (':'));
    if (port_position != std::string::npos && port_position > 0)
    {
        std::string port_string (string.substr (port_position + 1));
        try
        {
			uint16_t port;
			result = parse_port (port_string, port);
            if (!result)
            {
                boost::system::error_code ec;
                auto address (boost::asio::ip::address_v6::from_string (string.substr (0, port_position), ec));
                if (ec == 0)
                {
                    address_a = address;
                    port_a = port;
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
        }
        catch (...)
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
}

void paper::rpc::operator () (boost::network::http::server <paper::rpc>::request const & request, boost::network::http::server <paper::rpc>::response & response)
{
    if (request.method == "POST")
    {
        try
        {
            boost::property_tree::ptree request_l;
            std::stringstream istream (request.body);
            boost::property_tree::read_json (istream, request_l);
            std::string action (request_l.get <std::string> ("action"));
            if (node.config.logging.log_rpc ())
            {
                BOOST_LOG (node.log) << request.body;
            }
            if (action == "account_balance")
            {
                std::string account_text (request_l.get <std::string> ("account"));
                paper::uint256_union account;
                auto error (account.decode_base58check (account_text));
                if (!error)
                {
                    auto balance (node.balance (account));
                    boost::property_tree::ptree response_l;
                    response_l.put ("balance", balance.convert_to <std::string> ());
                    set_response (response, response_l);
                }
                else
                {
                    response = boost::network::http::server<paper::rpc>::response::stock_reply (boost::network::http::server<paper::rpc>::response::bad_request);
                    response.content = "Bad account number";
                }
            }
            else if (action == "account_weight")
            {
                std::string account_text (request_l.get <std::string> ("account"));
                paper::uint256_union account;
                auto error (account.decode_base58check (account_text));
                if (!error)
                {
                    auto balance (node.weight (account));
                    boost::property_tree::ptree response_l;
                    response_l.put ("weight", balance.convert_to <std::string> ());
                    set_response (response, response_l);
                }
                else
                {
                    response = boost::network::http::server<paper::rpc>::response::stock_reply (boost::network::http::server<paper::rpc>::response::bad_request);
                    response.content = "Bad account number";
                }
            }
            else if (action == "account_create")
            {
                if (config.enable_control)
                {
                    std::string wallet_text (request_l.get <std::string> ("wallet"));
                    paper::uint256_union wallet;
                    auto error (wallet.decode_hex (wallet_text));
                    if (!error)
                    {
                        auto existing (node.wallets.items.find (wallet));
                        if (existing != node.wallets.items.end ())
                        {
                            paper::keypair new_key;
                            existing->second->insert (new_key.prv);
                            boost::property_tree::ptree response_l;
                            response_l.put ("account", new_key.pub.to_base58check ());
                            set_response (response, response_l);
                        }
                        else
                        {
                            response = boost::network::http::server<paper::rpc>::response::stock_reply (boost::network::http::server<paper::rpc>::response::bad_request);
                            response.content = "Wallet not found";
                        }
                    }
                    else
                    {
                        response = boost::network::http::server<paper::rpc>::response::stock_reply (boost::network::http::server<paper::rpc>::response::bad_request);
                        response.content = "Bad wallet number";
                    }
                }
                else
                {
                    response = boost::network::http::server<paper::rpc>::response::stock_reply (boost::network::http::server<paper::rpc>::response::bad_request);
                    response.content = "RPC control is disabled";
                }
            }
            else if (action == "wallet_contains")
            {
                std::string account_text (request_l.get <std::string> ("account"));
                std::string wallet_text (request_l.get <std::string> ("wallet"));
                paper::uint256_union account;
                auto error (account.decode_base58check (account_text));
                if (!error)
                {
                    paper::uint256_union wallet;
                    auto error (wallet.decode_hex (wallet_text));
                    if (!error)
                    {
                        auto existing (node.wallets.items.find (wallet));
                        if (existing != node.wallets.items.end ())
                        {
							paper::transaction transaction (node.store.environment, nullptr, false);
                            auto exists (existing->second->store.find (transaction, account) != existing->second->store.end ());
                            boost::property_tree::ptree response_l;
                            response_l.put ("exists", exists ? "1" : "0");
                            set_response (response, response_l);
                        }
                        else
                        {
                            response = boost::network::http::server<paper::rpc>::response::stock_reply (boost::network::http::server<paper::rpc>::response::bad_request);
                            response.content = "Wallet not found";
                        }
                    }
                    else
                    {
                        response = boost::network::http::server<paper::rpc>::response::stock_reply (boost::network::http::server<paper::rpc>::response::bad_request);
                        response.content = "Bad wallet number";
                    }
                }
                else
                {
                    response = boost::network::http::server<paper::rpc>::response::stock_reply (boost::network::http::server<paper::rpc>::response::bad_request);
                    response.content = "Bad account number";
                }
            }
            else if (action == "account_list")
            {
                std::string wallet_text (request_l.get <std::string> ("wallet"));
                paper::uint256_union wallet;
                auto error (wallet.decode_hex (wallet_text));
                if (!error)
                {
                    auto existing (node.wallets.items.find (wallet));
                    if (existing != node.wallets.items.end ())
                    {
                        boost::property_tree::ptree response_l;
                        boost::property_tree::ptree accounts;
						paper::transaction transaction (node.store.environment, nullptr, false);
                        for (auto i (existing->second->store.begin (transaction)), j (existing->second->store.end ()); i != j; ++i)
                        {
                            boost::property_tree::ptree entry;
                            entry.put ("", paper::uint256_union (i->first).to_base58check ());
                            accounts.push_back (std::make_pair ("", entry));
                        }
                        response_l.add_child ("accounts", accounts);
                        set_response (response, response_l);
                    }
                    else
                    {
                        response = boost::network::http::server<paper::rpc>::response::stock_reply (boost::network::http::server<paper::rpc>::response::bad_request);
                        response.content = "Wallet not found";
                    }
                }
                else
                {
                    response = boost::network::http::server<paper::rpc>::response::stock_reply (boost::network::http::server<paper::rpc>::response::bad_request);
                    response.content = "Bad wallet number";
                }
            }
            else if (action == "wallet_add")
            {
                if (config.enable_control)
                {
                    std::string key_text (request_l.get <std::string> ("key"));
                    std::string wallet_text (request_l.get <std::string> ("wallet"));
                    paper::private_key key;
                    auto error (key.decode_hex (key_text));
                    if (!error)
                    {
                        paper::uint256_union wallet;
                        auto error (wallet.decode_hex (wallet_text));
                        if (!error)
                        {
                            auto existing (node.wallets.items.find (wallet));
                            if (existing != node.wallets.items.end ())
                            {
								paper::transaction transaction (node.store.environment, nullptr, true);
                                existing->second->store.insert (transaction, key);
                                paper::public_key pub;
                                ed25519_publickey (key.bytes.data (), pub.bytes.data ());
                                boost::property_tree::ptree response_l;
                                response_l.put ("account", pub.to_base58check ());
                                set_response (response, response_l);
                            }
                            else
                            {
                                response = boost::network::http::server<paper::rpc>::response::stock_reply (boost::network::http::server<paper::rpc>::response::bad_request);
                                response.content = "Wallet not found";
                            }
                        }
                        else
                        {
                            response = boost::network::http::server<paper::rpc>::response::stock_reply (boost::network::http::server<paper::rpc>::response::bad_request);
                            response.content = "Bad wallet number";
                        }
                    }
                    else
                    {
                        response = boost::network::http::server<paper::rpc>::response::stock_reply (boost::network::http::server<paper::rpc>::response::bad_request);
                        response.content = "Bad private key";
                    }
                }
                else
                {
                    response = boost::network::http::server<paper::rpc>::response::stock_reply (boost::network::http::server<paper::rpc>::response::bad_request);
                    response.content = "RPC control is disabled";
                }
            }
            else if (action == "wallet_key_valid")
            {
                if (config.enable_control)
                {
                    std::string wallet_text (request_l.get <std::string> ("wallet"));
                    paper::uint256_union wallet;
                    auto error (wallet.decode_hex (wallet_text));
                    if (!error)
                    {
                        auto existing (node.wallets.items.find (wallet));
                        if (existing != node.wallets.items.end ())
                        {
							paper::transaction transaction (node.store.environment, nullptr, false);
                            auto valid (existing->second->store.valid_password (transaction));
                            boost::property_tree::ptree response_l;
                            response_l.put ("valid", valid ? "1" : "0");
                            set_response (response, response_l);
                        }
                        else
                        {
                            response = boost::network::http::server<paper::rpc>::response::stock_reply (boost::network::http::server<paper::rpc>::response::bad_request);
                            response.content = "Wallet not found";
                        }
                    }
                    else
                    {
                        response = boost::network::http::server<paper::rpc>::response::stock_reply (boost::network::http::server<paper::rpc>::response::bad_request);
                        response.content = "Bad wallet number";
                    }
                }
                else
                {
                    response = boost::network::http::server<paper::rpc>::response::stock_reply (boost::network::http::server<paper::rpc>::response::bad_request);
                    response.content = "RPC control is disabled";
                }
            }
            else if (action == "validate_account_number")
            {
                std::string account_text (request_l.get <std::string> ("account"));
                paper::uint256_union account;
                auto error (account.decode_base58check (account_text));
                boost::property_tree::ptree response_l;
                response_l.put ("valid", error ? "0" : "1");
                set_response (response, response_l);
            }
            else if (action == "send")
            {
                if (config.enable_control)
                {
                    std::string wallet_text (request_l.get <std::string> ("wallet"));
                    paper::uint256_union wallet;
                    auto error (wallet.decode_hex (wallet_text));
                    if (!error)
                    {
                        auto existing (node.wallets.items.find (wallet));
                        if (existing != node.wallets.items.end ())
                        {
							std::string source_text (request_l.get <std::string> ("source"));
							paper::account source;
							auto error (source.decode_base58check (source_text));
							if (!error)
							{
								std::string destination_text (request_l.get <std::string> ("destination"));
								paper::account destination;
								auto error (destination.decode_base58check (destination_text));
								if (!error)
								{
									std::string amount_text (request_l.get <std::string> ("amount"));
									paper::amount amount;
									auto error (amount.decode_dec (amount_text));
									if (!error)
									{
										bool error (existing->second->send_sync (source, destination, amount.number ()));
										boost::property_tree::ptree response_l;
										response_l.put ("sent", error ? "0" : "1");
										set_response (response, response_l);
									}
									else
									{
										response = boost::network::http::server<paper::rpc>::response::stock_reply (boost::network::http::server<paper::rpc>::response::bad_request);
										response.content = "Bad amount format";
									}
								}
								else
								{
									response = boost::network::http::server<paper::rpc>::response::stock_reply (boost::network::http::server<paper::rpc>::response::bad_request);
									response.content = "Bad destination account";
								}
							}
							else
							{
								response = boost::network::http::server<paper::rpc>::response::stock_reply (boost::network::http::server<paper::rpc>::response::bad_request);
								response.content = "Bad source account";
							}
                        }
                        else
                        {
                            response = boost::network::http::server<paper::rpc>::response::stock_reply (boost::network::http::server<paper::rpc>::response::bad_request);
                            response.content = "Wallet not found";
                        }
                    }
                    else
                    {
                        response = boost::network::http::server<paper::rpc>::response::stock_reply (boost::network::http::server<paper::rpc>::response::bad_request);
                        response.content = "Bad wallet number";
                    }
                }
                else
                {
                    response = boost::network::http::server<paper::rpc>::response::stock_reply (boost::network::http::server<paper::rpc>::response::bad_request);
                    response.content = "RPC control is disabled";
                }
            }
            else if (action == "password_valid")
            {
                std::string wallet_text (request_l.get <std::string> ("wallet"));
                paper::uint256_union wallet;
                auto error (wallet.decode_hex (wallet_text));
                if (!error)
                {
                    auto existing (node.wallets.items.find (wallet));
                    if (existing != node.wallets.items.end ())
                    {
						paper::transaction transaction (node.store.environment, nullptr, false);
                        boost::property_tree::ptree response_l;
                        response_l.put ("valid", existing->second->store.valid_password (transaction) ? "1" : "0");
                        set_response (response, response_l);
                    }
                    else
                    {
                        response = boost::network::http::server<paper::rpc>::response::stock_reply (boost::network::http::server<paper::rpc>::response::bad_request);
                        response.content = "Wallet not found";
                    }
                }
                else
                {
                    response = boost::network::http::server<paper::rpc>::response::stock_reply (boost::network::http::server<paper::rpc>::response::bad_request);
                    response.content = "Bad account number";
                }
            }
            else if (action == "password_change")
            {
                std::string wallet_text (request_l.get <std::string> ("wallet"));
                paper::uint256_union wallet;
                auto error (wallet.decode_hex (wallet_text));
                if (!error)
                {
                    auto existing (node.wallets.items.find (wallet));
                    if (existing != node.wallets.items.end ())
                    {
						paper::transaction transaction (node.store.environment, nullptr, true);
                        boost::property_tree::ptree response_l;
                        std::string password_text (request_l.get <std::string> ("password"));
                        auto error (existing->second->store.rekey (transaction, password_text));
                        response_l.put ("changed", error ? "0" : "1");
                        set_response (response, response_l);
                    }
                    else
                    {
                        response = boost::network::http::server<paper::rpc>::response::stock_reply (boost::network::http::server<paper::rpc>::response::bad_request);
                        response.content = "Wallet not found";
                    }
                }
                else
                {
                    response = boost::network::http::server<paper::rpc>::response::stock_reply (boost::network::http::server<paper::rpc>::response::bad_request);
                    response.content = "Bad account number";
                }
            }
            else if (action == "password_enter")
            {
                std::string wallet_text (request_l.get <std::string> ("wallet"));
                paper::uint256_union wallet;
                auto error (wallet.decode_hex (wallet_text));
                if (!error)
                {
                    auto existing (node.wallets.items.find (wallet));
                    if (existing != node.wallets.items.end ())
                    {
						paper::transaction transaction (node.store.environment, nullptr, false);
                        boost::property_tree::ptree response_l;
                        std::string password_text (request_l.get <std::string> ("password"));
                        existing->second->store.enter_password (transaction, password_text);
                        response_l.put ("valid", existing->second->store.valid_password (transaction) ? "1" : "0");
                        set_response (response, response_l);
                    }
                    else
                    {
                        response = boost::network::http::server<paper::rpc>::response::stock_reply (boost::network::http::server<paper::rpc>::response::bad_request);
                        response.content = "Wallet not found";
                    }
                }
                else
                {
                    response = boost::network::http::server<paper::rpc>::response::stock_reply (boost::network::http::server<paper::rpc>::response::bad_request);
                    response.content = "Bad account number";
                }
            }
            else if (action == "representative")
            {
                std::string wallet_text (request_l.get <std::string> ("wallet"));
                paper::uint256_union wallet;
                auto error (wallet.decode_hex (wallet_text));
                if (!error)
                {
                    auto existing (node.wallets.items.find (wallet));
                    if (existing != node.wallets.items.end ())
                    {
						paper::transaction transaction (node.store.environment, nullptr, false);
                        boost::property_tree::ptree response_l;
                        response_l.put ("representative", existing->second->store.representative (transaction).to_base58check ());
                        set_response (response, response_l);
                    }
                    else
                    {
                        response = boost::network::http::server<paper::rpc>::response::stock_reply (boost::network::http::server<paper::rpc>::response::bad_request);
                        response.content = "Wallet not found";
                    }
                }
                else
                {
                    response = boost::network::http::server<paper::rpc>::response::stock_reply (boost::network::http::server<paper::rpc>::response::bad_request);
                    response.content = "Bad account number";
                }
            }
            else if (action == "representative_set")
            {
                std::string wallet_text (request_l.get <std::string> ("wallet"));
                paper::uint256_union wallet;
                auto error (wallet.decode_hex (wallet_text));
                if (!error)
                {
                    auto existing (node.wallets.items.find (wallet));
                    if (existing != node.wallets.items.end ())
                    {
                        std::string representative_text (request_l.get <std::string> ("representative"));
                        paper::account representative;
                        auto error (representative.decode_base58check (representative_text));
						if (!error)
						{
							paper::transaction transaction (node.store.environment, nullptr, true);
							existing->second->store.representative_set (transaction, representative);
							boost::property_tree::ptree response_l;
							response_l.put ("set", "1");
							set_response (response, response_l);
						}
						else
						{
							response = boost::network::http::server<paper::rpc>::response::stock_reply (boost::network::http::server<paper::rpc>::response::bad_request);
							response.content = "Invalid account number";
						}
                    }
                    else
                    {
                        response = boost::network::http::server<paper::rpc>::response::stock_reply (boost::network::http::server<paper::rpc>::response::bad_request);
                        response.content = "Wallet not found";
                    }
                }
                else
                {
                    response = boost::network::http::server<paper::rpc>::response::stock_reply (boost::network::http::server<paper::rpc>::response::bad_request);
                    response.content = "Bad account number";
                }
            }
            else if (action == "wallet_create")
            {
                paper::keypair wallet_id;
                auto wallet (node.wallets.create (wallet_id.prv));
                boost::property_tree::ptree response_l;
                response_l.put ("wallet", wallet_id.prv.to_string ());
                set_response (response, response_l);
            }
            else if (action == "wallet_export")
            {
                std::string wallet_text (request_l.get <std::string> ("wallet"));
                paper::uint256_union wallet;
                auto error (wallet.decode_hex (wallet_text));
                if (!error)
                {
                    auto existing (node.wallets.items.find (wallet));
                    if (existing != node.wallets.items.end ())
                    {
						paper::transaction transaction (node.store.environment, nullptr, false);
                        std::string json;
                        existing->second->store.serialize_json (transaction, json);
                        boost::property_tree::ptree response_l;
                        response_l.put ("json", json);
                        set_response (response, response_l);
                    }
                    else
                    {
                        response = boost::network::http::server<paper::rpc>::response::stock_reply (boost::network::http::server<paper::rpc>::response::bad_request);
                        response.content = "Wallet not found";
                    }
                }
                else
                {
                    response = boost::network::http::server<paper::rpc>::response::stock_reply (boost::network::http::server<paper::rpc>::response::bad_request);
                    response.content = "Bad account number";
                }
            }
            else if (action == "wallet_destroy")
            {
                std::string wallet_text (request_l.get <std::string> ("wallet"));
                paper::uint256_union wallet;
                auto error (wallet.decode_hex (wallet_text));
                if (!error)
                {
                    auto existing (node.wallets.items.find (wallet));
                    if (existing != node.wallets.items.end ())
                    {
                        node.wallets.destroy (wallet);
                        boost::property_tree::ptree response_l;
                        set_response (response, response_l);
                    }
                    else
                    {
                        response = boost::network::http::server<paper::rpc>::response::stock_reply (boost::network::http::server<paper::rpc>::response::bad_request);
                        response.content = "Wallet not found";
                    }
                }
                else
                {
                    response = boost::network::http::server<paper::rpc>::response::stock_reply (boost::network::http::server<paper::rpc>::response::bad_request);
                    response.content = "Bad wallet number";
                }
            }
            else if (action == "account_move")
            {
                std::string wallet_text (request_l.get <std::string> ("wallet"));
                std::string source_text (request_l.get <std::string> ("source"));
                auto accounts_text (request_l.get_child ("accounts"));
                paper::uint256_union wallet;
                auto error (wallet.decode_hex (wallet_text));
                if (!error)
                {
                    auto existing (node.wallets.items.find (wallet));
                    if (existing != node.wallets.items.end ())
                    {
                        auto wallet (existing->second);
                        paper::uint256_union source;
                        auto error (source.decode_hex (source_text));
                        if (!error)
                        {
                            auto existing (node.wallets.items.find (source));
                            if (existing != node.wallets.items.end ())
                            {
                                auto source (existing->second);
                                std::vector <paper::public_key> accounts;
                                for (auto i (accounts_text.begin ()), n (accounts_text.end ()); i != n; ++i)
                                {
                                    paper::public_key account;
                                    account.decode_hex (i->second.get <std::string> (""));
                                    accounts.push_back (account);
                                }
								paper::transaction transaction (node.store.environment, nullptr, true);
                                auto error (wallet->store.move (transaction, source->store, accounts));
                                boost::property_tree::ptree response_l;
                                response_l.put ("moved", error ? "0" : "1");
                                set_response (response, response_l);
                            }
                            else
                            {
                                response = boost::network::http::server<paper::rpc>::response::stock_reply (boost::network::http::server<paper::rpc>::response::bad_request);
                                response.content = "Source not found";
                            }
                        }
                        else
                        {
                            response = boost::network::http::server<paper::rpc>::response::stock_reply (boost::network::http::server<paper::rpc>::response::bad_request);
                            response.content = "Bad source number";
                        }
                    }
                    else
                    {
                        response = boost::network::http::server<paper::rpc>::response::stock_reply (boost::network::http::server<paper::rpc>::response::bad_request);
                        response.content = "Wallet not found";
                    }
                }
                else
                {
                    response = boost::network::http::server<paper::rpc>::response::stock_reply (boost::network::http::server<paper::rpc>::response::bad_request);
                    response.content = "Bad wallet number";
                }
            }
            else if (action == "block")
            {
                std::string hash_text (request_l.get <std::string> ("hash"));
				paper::uint256_union hash;
                auto error (hash.decode_hex (hash_text));
                if (!error)
                {
					paper::transaction transaction (node.store.environment, nullptr, false);
					auto block (node.store.block_get (transaction, hash));
					if (block != nullptr)
                    {
                        boost::property_tree::ptree response_l;
						std::string contents;
						block->serialize_json (contents);
						response_l.put ("contents", contents);
                        set_response (response, response_l);
                    }
                    else
                    {
                        response = boost::network::http::server<paper::rpc>::response::stock_reply (boost::network::http::server<paper::rpc>::response::bad_request);
                        response.content = "Block not found";
                    }
                }
                else
                {
                    response = boost::network::http::server<paper::rpc>::response::stock_reply (boost::network::http::server<paper::rpc>::response::bad_request);
                    response.content = "Bad hash number";
                }
            }
            else if (action == "process")
            {
                std::string block_text (request_l.get <std::string> ("block"));
				boost::property_tree::ptree block_l;
				std::stringstream block_stream (block_text);
				boost::property_tree::read_json (block_stream, block_l);
				auto block (paper::deserialize_block_json (block_l));
				if (block != nullptr)
				{
					node.process_receive_republish (std::move (block), 0);
                    boost::property_tree::ptree response_l;
					set_response (response, response_l);
				}
				else
				{
					response = boost::network::http::server<paper::rpc>::response::stock_reply (boost::network::http::server<paper::rpc>::response::bad_request);
					response.content = "Block is invalid";
				}
            }
            else if (action == "price")
            {
                std::string account_text (request_l.get <std::string> ("account"));
                paper::uint256_union account;
                auto error (account.decode_base58check (account_text));
                if (!error)
                {
					auto amount_text (request_l.get <std::string> ("amount"));
					try
					{
						auto amount (std::stoi (amount_text));
						if (amount < 1000)
						{
							auto balance (node.balance (account));
							auto price (node.price (balance, amount));
							boost::property_tree::ptree response_l;
							response_l.put ("price", std::to_string (price));
							set_response (response, response_l);
						}
						else
						{
							response = boost::network::http::server<paper::rpc>::response::stock_reply (boost::network::http::server<paper::rpc>::response::bad_request);
							response.content = "Cannot purchase more than 1000";
						}
					}
					catch (std::invalid_argument const &)
					{
						response = boost::network::http::server<paper::rpc>::response::stock_reply (boost::network::http::server<paper::rpc>::response::bad_request);
						response.content = "Invalid amount number";
					}
					catch (std::out_of_range const &)
					{
						response = boost::network::http::server<paper::rpc>::response::stock_reply (boost::network::http::server<paper::rpc>::response::bad_request);
						response.content = "Invalid amount";
					}
                }
                else
                {
                    response = boost::network::http::server<paper::rpc>::response::stock_reply (boost::network::http::server<paper::rpc>::response::bad_request);
                    response.content = "Bad account number";
                }
            }
            else if (action == "frontiers")
            {
				boost::property_tree::ptree response_l;
				boost::property_tree::ptree frontiers;
				paper::transaction transaction (node.store.environment, nullptr, false);
				for (auto i (node.store.latest_begin (transaction)), n (node.store.latest_end ()); i != n; ++i)
				{
					frontiers.put (paper::account (i->first).to_base58check (), paper::account_info (i->second).head.to_string ());
				}
				response_l.add_child ("frontiers", frontiers);
				set_response (response, response_l);
            }
            else if (action == "search_pending")
            {
				std::string wallet_text (request_l.get <std::string> ("wallet"));
                paper::uint256_union wallet;
                auto error (wallet.decode_hex (wallet_text));
                if (!error)
                {
                    auto existing (node.wallets.items.find (wallet));
                    if (existing != node.wallets.items.end ())
                    {
						auto error (existing->second->search_pending ());
						boost::property_tree::ptree response_l;
						response_l.put ("started", !error);
						set_response (response, response_l);
                    }
                    else
                    {
                        response = boost::network::http::server<paper::rpc>::response::stock_reply (boost::network::http::server<paper::rpc>::response::bad_request);
                        response.content = "Wallet not found";
                    }
				}
            }
            else if (action == "keepalive")
            {
                std::string address_text (request_l.get <std::string> ("address"));
				std::string port_text (request_l.get <std::string> ("port"));
				uint16_t port;
				if (!parse_port (port_text, port))
				{
					node.keepalive (address_text, port);
					boost::property_tree::ptree response_l;
					set_response (response, response_l);
				}
				else
				{
                    response = boost::network::http::server<paper::rpc>::response::stock_reply (boost::network::http::server<paper::rpc>::response::bad_request);
                    response.content = "Invalid port";
				}
            }
            else
            {
                response = boost::network::http::server<paper::rpc>::response::stock_reply (boost::network::http::server<paper::rpc>::response::bad_request);
                response.content = "Unknown command";
            }
        }
        catch (std::runtime_error const &)
        {
            response = boost::network::http::server<paper::rpc>::response::stock_reply (boost::network::http::server<paper::rpc>::response::bad_request);
            response.content = "Unable to parse JSON";
        }
    }
    else
    {
        response = boost::network::http::server<paper::rpc>::response::stock_reply (boost::network::http::server<paper::rpc>::response::method_not_allowed);
        response.content = "Can only POST requests";
    }
}

namespace
{
class rollback_visitor : public paper::block_visitor
{
public:
    rollback_visitor (paper::ledger & ledger_a) :
    ledger (ledger_a)
    {
    }
    void send_block (paper::send_block const & block_a) override
    {
		auto hash (block_a.hash ());
        paper::receivable receivable;
		paper::transaction transaction (ledger.store.environment, nullptr, true);
		while (ledger.store.pending_get (transaction, hash, receivable))
		{
			ledger.rollback (transaction, ledger.latest (transaction, block_a.hashables.destination));
		}
        paper::account_info info;
        ledger.store.account_get (transaction, receivable.source, info);
		ledger.store.pending_del (transaction, hash);
        ledger.change_latest (transaction, receivable.source, block_a.hashables.previous, info.rep_block, ledger.balance (transaction, block_a.hashables.previous));
		ledger.store.block_del (transaction, hash);
    }
    void receive_block (paper::receive_block const & block_a) override
    {
		paper::transaction transaction (ledger.store.environment, nullptr, true);
		auto hash (block_a.hash ());
        auto representative (ledger.representative (transaction, block_a.hashables.source));
        auto amount (ledger.amount (transaction, block_a.hashables.source));
        auto destination_account (ledger.account (transaction, hash));
		ledger.move_representation (transaction, ledger.representative (transaction, hash), representative, amount);
        ledger.change_latest (transaction, destination_account, block_a.hashables.previous, representative, ledger.balance (transaction, block_a.hashables.previous));
		ledger.store.block_del (transaction, hash);
        ledger.store.pending_put (transaction, block_a.hashables.source, {ledger.account (transaction, block_a.hashables.source), amount, destination_account});
    }
    void open_block (paper::open_block const & block_a) override
    {
		paper::transaction transaction (ledger.store.environment, nullptr, true);
		auto hash (block_a.hash ());
        auto representative (ledger.representative (transaction, block_a.hashables.source));
        auto amount (ledger.amount (transaction, block_a.hashables.source));
        auto destination_account (ledger.account (transaction, hash));
		ledger.move_representation (transaction, ledger.representative (transaction, hash), representative, amount);
        ledger.change_latest (transaction, destination_account, 0, representative, 0);
		ledger.store.block_del (transaction, hash);
        ledger.store.pending_put (transaction, block_a.hashables.source, {ledger.account (transaction, block_a.hashables.source), amount, destination_account});
    }
    void change_block (paper::change_block const & block_a) override
    {
		paper::transaction transaction (ledger.store.environment, nullptr, true);
        auto representative (ledger.representative (transaction, block_a.hashables.previous));
        auto account (ledger.account (transaction, block_a.hashables.previous));
        paper::account_info info;
        ledger.store.account_get (transaction, account, info);
		ledger.move_representation (transaction, block_a.representative (), representative, ledger.balance (transaction, block_a.hashables.previous));
		ledger.store.block_del (transaction, block_a.hash ());
        ledger.change_latest (transaction, account, block_a.hashables.previous, representative, info.balance);
    }
    paper::ledger & ledger;
};
}

bool paper::parse_endpoint (std::string const & string, paper::endpoint & endpoint_a)
{
    boost::asio::ip::address address;
    uint16_t port;
    auto result (parse_address_port (string, address, port));
    if (!result)
    {
        endpoint_a = paper::endpoint (address, port);
    }
    return result;
}

bool paper::parse_tcp_endpoint (std::string const & string, paper::tcp_endpoint & endpoint_a)
{
    boost::asio::ip::address address;
    uint16_t port;
    auto result (parse_address_port (string, address, port));
    if (!result)
    {
        endpoint_a = paper::tcp_endpoint (address, port);
    }
    return result;
}

paper::bulk_pull::bulk_pull () :
message (paper::message_type::bulk_pull)
{
}

void paper::bulk_pull::visit (paper::message_visitor & visitor_a) const
{
    visitor_a.bulk_pull (*this);
}

bool paper::bulk_pull::deserialize (paper::stream & stream_a)
{
	auto result (read_header (stream_a, version_max, version_using, version_min, type, extensions));
	assert (!result);
	assert (paper::message_type::bulk_pull == type);
    if (!result)
    {
        assert (type == paper::message_type::bulk_pull);
        result = read (stream_a, start);
        if (!result)
        {
            result = read (stream_a, end);
        }
    }
    return result;
}

void paper::bulk_pull::serialize (paper::stream & stream_a)
{
	write_header (stream_a);
    write (stream_a, start);
    write (stream_a, end);
}

paper::bulk_push::bulk_push () :
message (paper::message_type::bulk_push)
{
}

bool paper::bulk_push::deserialize (paper::stream & stream_a)
{
    auto result (read_header (stream_a, version_max, version_using, version_min, type, extensions));
    assert (!result);
    assert (paper::message_type::bulk_push == type);
    return result;
}

void paper::bulk_push::serialize (paper::stream & stream_a)
{
    write_header (stream_a);
}

void paper::bulk_push::visit (paper::message_visitor & visitor_a) const
{
    visitor_a.bulk_push (*this);
}

void paper::node::start ()
{
    network.receive ();
    ongoing_keepalive ();
    bootstrap.start ();
	backup_wallet ();
}

void paper::node::stop ()
{
    BOOST_LOG (log) << "Node stopping";
	conflicts.roots.clear ();
    network.stop ();
    bootstrap.stop ();
    service.stop ();
}

void paper::node::keepalive_preconfigured (std::vector <std::string> const & peers_a)
{
	for (auto i (peers_a.begin ()), n (peers_a.end ()); i != n; ++i)
	{
		keepalive (*i, paper::network::node_port);
	}
}

paper::block_hash paper::node::latest (paper::account const & account_a)
{
	paper::transaction transaction (store.environment, nullptr, false);
	return ledger.latest (transaction, account_a);
}

paper::uint128_t paper::node::balance (paper::account const & account_a)
{
	paper::transaction transaction (store.environment, nullptr, false);
	return ledger.account_balance (transaction, account_a);
}

paper::uint128_t paper::node::weight (paper::account const & account_a)
{
	paper::transaction transaction (store.environment, nullptr, false);
	return ledger.weight (transaction, account_a);
}

paper::account paper::node::representative (paper::account const & account_a)
{
	paper::transaction transaction (store.environment, nullptr, false);
	paper::account_info info;
	paper::account result (0);
	if (!store.account_get (transaction, account_a, info))
	{
		result = info.rep_block;
	}
	return result;
}

void paper::node::call_observers (paper::block const & block_a, paper::account const & account_a)
{
	for (auto & i: observers)
	{
		i (block_a, account_a);
	}
}

void paper::node::ongoing_keepalive ()
{
    keepalive_preconfigured (config.preconfigured_peers);
    auto peers_l (peers.purge_list (std::chrono::system_clock::now () - cutoff));
    for (auto i (peers_l.begin ()), j (peers_l.end ()); i != j && std::chrono::system_clock::now () - i->last_attempt > period; ++i)
    {
        network.send_keepalive (i->endpoint);
    }
	auto node_l (shared_from_this ());
    service.add (std::chrono::system_clock::now () + period, [node_l] () { node_l->ongoing_keepalive ();});
}

void paper::node::backup_wallet ()
{
	paper::transaction transaction (store.environment, nullptr, false);
	for (auto i (wallets.items.begin ()), n (wallets.items.end ()); i != n; ++i)
	{
		auto backup_path (application_path / "backup");
		boost::filesystem::create_directories (backup_path);
		i->second->store.write_backup (transaction, backup_path / (i->first.to_string () + ".json"));
	}
	auto this_l (shared ());
	service.add (std::chrono::system_clock::now () + backup_interval, [this_l] ()
	{
		this_l->backup_wallet ();
	});
}

int paper::node::price (paper::uint128_t const & balance_a, int amount_a)
{
	auto balance_l (balance_a);
	int result (0);
	for (auto i (0); i < amount_a; ++i)
	{
		auto units ((balance_l / paper::Gpaper_ratio).convert_to <double> ());
		auto unit_price (((free_cutoff - units) / free_cutoff) * price_max);
		result += std::min (std::max (0.0, unit_price), price_max);
		balance_l -= paper::Gpaper_ratio;
	}
	return result;
}

namespace
{
class confirmed_visitor : public paper::block_visitor
{
public:
    confirmed_visitor (paper::node & node_a) :
    node (node_a)
    {
    }
    void send_block (paper::send_block const & block_a) override
    {
        for (auto i (node.wallets.items.begin ()), n (node.wallets.items.end ()); i != n; ++i)
        {
			auto wallet (i->second);
			if (wallet->exists (block_a.hashables.destination))
			{
				paper::private_key prv;
				paper::account representative;
				auto error (false);
				{
					paper::transaction transaction (node.store.environment, nullptr, false);
					error = wallet->store.fetch (transaction, block_a.hashables.destination, prv);
					representative = wallet->store.representative (transaction);
				}
				if (!error)
				{
					auto block_l (std::shared_ptr <paper::send_block> (static_cast <paper::send_block *> (block_a.clone ().release ())));
					auto node_l (node.shared ());
					node.service.add (std::chrono::system_clock::now (), [block_l, prv, representative, wallet, node_l] ()
					{
						node_l->wallets.queue_wallet_action (block_l->hashables.destination, [block_l, prv, representative, wallet] ()
						{
							auto error (wallet->receive_action (*block_l, prv, representative));
							(void)error; // Might be interesting to view during debug
						});
					});
				}
				else
				{
					BOOST_LOG (node.log) << "While confirming, unable to fetch wallet key";
				}
			}
        }
    }
    void receive_block (paper::receive_block const &) override
    {
    }
    void open_block (paper::open_block const &) override
    {
    }
    void change_block (paper::change_block const &) override
    {
    }
    paper::node & node;
};
}

void paper::node::process_confirmed (paper::block const & confirmed_a)
{
    confirmed_visitor visitor (*this);
    confirmed_a.visit (visitor);
}

void paper::node::process_message (paper::message & message_a, paper::endpoint const & sender_a)
{
	network_message_visitor visitor (*this, sender_a);
	message_a.visit (visitor);
}

paper::bootstrap_initiator::bootstrap_initiator (paper::node & node_a) :
node (node_a),
in_progress (false),
warmed_up (false)
{
}

void paper::bootstrap_initiator::warmup (paper::endpoint const & endpoint_a)
{
	std::lock_guard <std::mutex> lock (mutex);
	if (!in_progress && warmed_up.size () < 2 && !in_progress && warmed_up.find (endpoint_a) == warmed_up.end ())
	{
		warmed_up.insert (endpoint_a);
		in_progress = true;
		notify_listeners ();
		initiate (endpoint_a);
	}
}

void paper::bootstrap_initiator::bootstrap (paper::endpoint const & endpoint_a)
{
	std::lock_guard <std::mutex> lock (mutex);
	if (!in_progress)
	{
		initiate (endpoint_a);
	}
}

void paper::bootstrap_initiator::bootstrap_any ()
{
    auto list (node.peers.bootstrap_candidates ());
    if (!list.empty ())
    {
        bootstrap (list [random_pool.GenerateWord32 (0, list.size () - 1)].endpoint);
    }
}

void paper::bootstrap_initiator::initiate (paper::endpoint const & endpoint_a)
{
	auto node_l (node.shared ());
    auto processor (std::make_shared <paper::bootstrap_client> (node_l, [node_l] ()
	{
		std::lock_guard <std::mutex> lock (node_l->bootstrap_initiator.mutex);
		node_l->bootstrap_initiator.in_progress = false;
		node_l->bootstrap_initiator.notify_listeners ();
	}));
    processor->run (paper::tcp_endpoint (endpoint_a.address (), endpoint_a.port ()));
}

void paper::bootstrap_initiator::notify_listeners ()
{
	for (auto & i: observers)
	{
		i (in_progress);
	}
}

paper::bootstrap_listener::bootstrap_listener (boost::asio::io_service & service_a, uint16_t port_a, paper::node & node_a) :
acceptor (service_a),
local (boost::asio::ip::tcp::endpoint (boost::asio::ip::address_v6::any (), port_a)),
service (service_a),
node (node_a)
{
}

void paper::bootstrap_listener::start ()
{
    acceptor.open (local.protocol ());
    acceptor.set_option (boost::asio::ip::tcp::acceptor::reuse_address (true));
    acceptor.bind (local);
    acceptor.listen ();
    accept_connection ();
}

void paper::bootstrap_listener::stop ()
{
    on = false;
    acceptor.close ();
}

void paper::bootstrap_listener::accept_connection ()
{
    auto socket (std::make_shared <boost::asio::ip::tcp::socket> (service));
    acceptor.async_accept (*socket, [this, socket] (boost::system::error_code const & ec)
    {
        accept_action (ec, socket);
    });
}

void paper::bootstrap_listener::accept_action (boost::system::error_code const & ec, std::shared_ptr <boost::asio::ip::tcp::socket> socket_a)
{
    if (!ec)
    {
        accept_connection ();
        auto connection (std::make_shared <paper::bootstrap_server> (socket_a, node.shared ()));
        connection->receive ();
    }
    else
    {
        BOOST_LOG (node.log) << boost::str (boost::format ("Error while accepting bootstrap connections: %1%") % ec.message ());
    }
}

paper::bootstrap_server::bootstrap_server (std::shared_ptr <boost::asio::ip::tcp::socket> socket_a, std::shared_ptr <paper::node> node_a) :
socket (socket_a),
node (node_a)
{
}

void paper::bootstrap_server::receive ()
{
    auto this_l (shared_from_this ());
    boost::asio::async_read (*socket, boost::asio::buffer (receive_buffer.data (), 8), [this_l] (boost::system::error_code const & ec, size_t size_a)
    {
        this_l->receive_header_action (ec, size_a);
    });
}

void paper::bootstrap_server::receive_header_action (boost::system::error_code const & ec, size_t size_a)
{
    if (!ec)
    {
        assert (size_a == 8);
		paper::bufferstream type_stream (receive_buffer.data (), size_a);
		uint8_t version_max;
		uint8_t version_using;
		uint8_t version_min;
		paper::message_type type;
		std::bitset <16> extensions;
		if (!paper::message::read_header (type_stream, version_max, version_using, version_min, type, extensions))
		{
			switch (type)
			{
				case paper::message_type::bulk_pull:
				{
					auto this_l (shared_from_this ());
					boost::asio::async_read (*socket, boost::asio::buffer (receive_buffer.data () + 8, sizeof (paper::uint256_union) + sizeof (paper::uint256_union)), [this_l] (boost::system::error_code const & ec, size_t size_a)
					{
						this_l->receive_bulk_pull_action (ec, size_a);
					});
					break;
				}
				case paper::message_type::frontier_req:
				{
					auto this_l (shared_from_this ());
					boost::asio::async_read (*socket, boost::asio::buffer (receive_buffer.data () + 8, sizeof (paper::uint256_union) + sizeof (uint32_t) + sizeof (uint32_t)), [this_l] (boost::system::error_code const & ec, size_t size_a)
					{
						this_l->receive_frontier_req_action (ec, size_a);
					});
					break;
				}
                case paper::message_type::bulk_push:
                {
                    add_request (std::unique_ptr <paper::message> (new paper::bulk_push));
                    break;
                }
				default:
				{
					if (node->config.logging.network_logging ())
					{
						BOOST_LOG (node->log) << boost::str (boost::format ("Received invalid type from bootstrap connection %1%") % static_cast <uint8_t> (type));
					}
					break;
				}
			}
		}
    }
    else
    {
        if (node->config.logging.network_logging ())
        {
            BOOST_LOG (node->log) << boost::str (boost::format ("Error while receiving type %1%") % ec.message ());
        }
    }
}

void paper::bootstrap_server::receive_bulk_pull_action (boost::system::error_code const & ec, size_t size_a)
{
    if (!ec)
    {
        std::unique_ptr <paper::bulk_pull> request (new paper::bulk_pull);
        paper::bufferstream stream (receive_buffer.data (), 8 + sizeof (paper::uint256_union) + sizeof (paper::uint256_union));
        auto error (request->deserialize (stream));
        if (!error)
        {
            if (node->config.logging.network_logging ())
            {
                BOOST_LOG (node->log) << boost::str (boost::format ("Received bulk pull for %1% down to %2%") % request->start.to_string () % request->end.to_string ());
            }
			add_request (std::unique_ptr <paper::message> (request.release ()));
            receive ();
        }
    }
}

void paper::bootstrap_server::receive_frontier_req_action (boost::system::error_code const & ec, size_t size_a)
{
	if (!ec)
	{
		std::unique_ptr <paper::frontier_req> request (new paper::frontier_req);
		paper::bufferstream stream (receive_buffer.data (), 8 + sizeof (paper::uint256_union) + sizeof (uint32_t) + sizeof (uint32_t));
		auto error (request->deserialize (stream));
		if (!error)
		{
			if (node->config.logging.network_logging ())
			{
				BOOST_LOG (node->log) << boost::str (boost::format ("Received frontier request for %1% with age %2%") % request->start.to_string () % request->age);
			}
			add_request (std::unique_ptr <paper::message> (request.release ()));
			receive ();
		}
	}
    else
    {
        if (node->config.logging.network_logging ())
        {
            BOOST_LOG (node->log) << boost::str (boost::format ("Error sending receiving frontier request %1%") % ec.message ());
        }
    }
}

void paper::bootstrap_server::add_request (std::unique_ptr <paper::message> message_a)
{
	std::lock_guard <std::mutex> lock (mutex);
    auto start (requests.empty ());
	requests.push (std::move (message_a));
	if (start)
	{
		run_next ();
	}
}

void paper::bootstrap_server::finish_request ()
{
	std::lock_guard <std::mutex> lock (mutex);
	requests.pop ();
	if (!requests.empty ())
	{
		run_next ();
	}
}

namespace
{
class request_response_visitor : public paper::message_visitor
{
public:
    request_response_visitor (std::shared_ptr <paper::bootstrap_server> connection_a) :
    connection (connection_a)
    {
    }
    void keepalive (paper::keepalive const &) override
    {
        assert (false);
    }
    void publish (paper::publish const &) override
    {
        assert (false);
    }
    void confirm_req (paper::confirm_req const &) override
    {
        assert (false);
    }
    void confirm_ack (paper::confirm_ack const &) override
    {
        assert (false);
    }
    void bulk_pull (paper::bulk_pull const &) override
    {
        auto response (std::make_shared <paper::bulk_pull_server> (connection, std::unique_ptr <paper::bulk_pull> (static_cast <paper::bulk_pull *> (connection->requests.front ().release ()))));
        response->send_next ();
    }
    void bulk_push (paper::bulk_push const &) override
    {
        auto response (std::make_shared <paper::bulk_push_server> (connection));
        response->receive ();
    }
    void frontier_req (paper::frontier_req const &) override
    {
        auto response (std::make_shared <paper::frontier_req_server> (connection, std::unique_ptr <paper::frontier_req> (static_cast <paper::frontier_req *> (connection->requests.front ().release ()))));
        response->send_next ();
    }
    std::shared_ptr <paper::bootstrap_server> connection;
};
}

void paper::bootstrap_server::run_next ()
{
	assert (!requests.empty ());
    request_response_visitor visitor (shared_from_this ());
    requests.front ()->visit (visitor);
}

void paper::bulk_pull_server::set_current_end ()
{
    assert (request != nullptr);
	paper::transaction transaction (connection->node->store.environment, nullptr, false);
	if (!connection->node->store.block_exists (transaction, request->end))
	{
		if (connection->node->config.logging.bulk_pull_logging ())
		{
			BOOST_LOG (connection->node->log) << boost::str (boost::format ("Bulk pull end block doesn't exist: %1%, sending everything") % request->end.to_string ());
		}
		request->end.clear ();
	}
	paper::account_info info;
	auto no_address (connection->node->store.account_get (transaction, request->start, info));
	if (no_address)
	{
		if (connection->node->config.logging.bulk_pull_logging ())
		{
			BOOST_LOG (connection->node->log) << boost::str (boost::format ("Request for unknown account: %1%") % request->start.to_base58check ());
		}
		current = request->end;
	}
	else
	{
		if (!request->end.is_zero ())
		{
			auto account (connection->node->ledger.account (transaction, request->end));
			if (account == request->start)
			{
				current = info.head;
			}
			else
			{
				current = request->end;
			}
		}
		else
		{
			current = info.head;
		}
	}
}

void paper::bulk_pull_server::send_next ()
{
    std::unique_ptr <paper::block> block (get_next ());
    if (block != nullptr)
    {
        {
            send_buffer.clear ();
            paper::vectorstream stream (send_buffer);
            paper::serialize_block (stream, *block);
        }
        auto this_l (shared_from_this ());
        if (connection->node->config.logging.bulk_pull_logging ())
        {
            BOOST_LOG (connection->node->log) << boost::str (boost::format ("Sending block: %1%") % block->hash ().to_string ());
        }
        async_write (*connection->socket, boost::asio::buffer (send_buffer.data (), send_buffer.size ()), [this_l] (boost::system::error_code const & ec, size_t size_a)
        {
            this_l->sent_action (ec, size_a);
        });
    }
    else
    {
        send_finished ();
    }
}

std::unique_ptr <paper::block> paper::bulk_pull_server::get_next ()
{
    std::unique_ptr <paper::block> result;
    if (current != request->end)
    {
		paper::transaction transaction (connection->node->store.environment, nullptr, false);
        result = connection->node->store.block_get (transaction, current);
        assert (result != nullptr);
        auto previous (result->previous ());
        if (!previous.is_zero ())
        {
            current = previous;
        }
        else
        {
            request->end = current;
        }
    }
    return result;
}

void paper::bulk_pull_server::sent_action (boost::system::error_code const & ec, size_t size_a)
{
    if (!ec)
    {
        send_next ();
    }
	else
	{
		BOOST_LOG (connection->node->log) << boost::str (boost::format ("Unable to bulk send block: %1%") % ec.message ());
	}
}

void paper::bulk_pull_server::send_finished ()
{
    send_buffer.clear ();
    send_buffer.push_back (static_cast <uint8_t> (paper::block_type::not_a_block));
    auto this_l (shared_from_this ());
    if (connection->node->config.logging.network_logging ())
    {
        BOOST_LOG (connection->node->log) << "Bulk sending finished";
    }
    async_write (*connection->socket, boost::asio::buffer (send_buffer.data (), 1), [this_l] (boost::system::error_code const & ec, size_t size_a)
    {
        this_l->no_block_sent (ec, size_a);
    });
}

void paper::bulk_pull_server::no_block_sent (boost::system::error_code const & ec, size_t size_a)
{
    if (!ec)
    {
        assert (size_a == 1);
		connection->finish_request ();
    }
	else
	{
		BOOST_LOG (connection->node->log) << "Unable to send not-a-block";
	}
}

paper::bootstrap_client::bootstrap_client (std::shared_ptr <paper::node> node_a, std::function <void ()> const & completion_action_a) :
node (node_a),
socket (node_a->network.service),
completion_action (completion_action_a)
{
}

paper::bootstrap_client::~bootstrap_client ()
{
	if (node->config.logging.network_logging ())
	{
		BOOST_LOG (node->log) << "Exiting bootstrap client";
	}
	completion_action ();
}

void paper::bootstrap_client::run (boost::asio::ip::tcp::endpoint const & endpoint_a)
{
    if (node->config.logging.network_logging ())
    {
        BOOST_LOG (node->log) << boost::str (boost::format ("Initiating bootstrap connection to %1%") % endpoint_a);
    }
    auto this_l (shared_from_this ());
    socket.async_connect (endpoint_a, [this_l, endpoint_a] (boost::system::error_code const & ec)
    {
		if (!ec)
		{
			this_l->connect_action ();
		}
		else
		{
			if (this_l->node->config.logging.network_logging ())
			{
				BOOST_LOG (this_l->node->log) << boost::str (boost::format ("Error initiating bootstrap connection %1%") % ec.message ());
			}
			this_l->node->peers.bootstrap_failed (paper::endpoint (endpoint_a.address (), endpoint_a.port ()));
		}
    });
}

void paper::bootstrap_client::connect_action ()
{
	std::unique_ptr <paper::frontier_req> request (new paper::frontier_req);
	request->start.clear ();
	request->age = std::numeric_limits <decltype (request->age)>::max ();
	request->count = std::numeric_limits <decltype (request->age)>::max ();
	auto send_buffer (std::make_shared <std::vector <uint8_t>> ());
	{
		paper::vectorstream stream (*send_buffer);
		request->serialize (stream);
	}
	if (node->config.logging.network_logging ())
	{
		BOOST_LOG (node->log) << boost::str (boost::format ("Initiating frontier request for %1% age %2% count %3%") % request->start.to_string () % request->age % request->count);
	}
	auto this_l (shared_from_this ());
	boost::asio::async_write (socket, boost::asio::buffer (send_buffer->data (), send_buffer->size ()), [this_l, send_buffer] (boost::system::error_code const & ec, size_t size_a)
	{
		this_l->sent_request (ec, size_a);
	});
}

void paper::bootstrap_client::sent_request (boost::system::error_code const & ec, size_t size_a)
{
    if (!ec)
    {
        auto this_l (shared_from_this ());
        auto client_l (std::make_shared <paper::frontier_req_client> (this_l));
        client_l->receive_frontier ();
    }
    else
    {
        if (node->config.logging.network_logging ())
        {
            BOOST_LOG (node->log) << boost::str (boost::format ("Error while sending bootstrap request %1%") % ec.message ());
        }
    }
}

void paper::bulk_pull_client::request ()
{
    if (current != end)
    {
        paper::bulk_pull req;
        req.start = current->first;
        req.end = current->second;
        ++current;
        auto buffer (std::make_shared <std::vector <uint8_t>> ());
        {
            paper::vectorstream stream (*buffer);
            req.serialize (stream);
        }
		if (connection->connection->node->config.logging.network_logging ())
		{
			BOOST_LOG (connection->connection->node->log) << boost::str (boost::format ("Requesting account %1% down to %2%") % req.start.to_string () % req.end.to_string ());
		}
        auto this_l (shared_from_this ());
        boost::asio::async_write (connection->connection->socket, boost::asio::buffer (buffer->data (), buffer->size ()), [this_l, buffer] (boost::system::error_code const & ec, size_t size_a)
            {
                if (!ec)
                {
                    this_l->receive_block ();
                }
                else
                {
                    BOOST_LOG (this_l->connection->connection->node->log) << boost::str (boost::format ("Error sending bulk pull request %1%") % ec.message ());
                }
            });
    }
    else
    {
        process_end ();
        connection->completed_pulls ();
    }
}

void paper::bulk_pull_client::receive_block ()
{
    auto this_l (shared_from_this ());
    boost::asio::async_read (connection->connection->socket, boost::asio::buffer (receive_buffer.data (), 1), [this_l] (boost::system::error_code const & ec, size_t size_a)
    {
        if (!ec)
        {
            this_l->received_type ();
        }
        else
        {
            BOOST_LOG (this_l->connection->connection->node->log) << boost::str (boost::format ("Error receiving block type %1%") % ec.message ());
        }
    });
}

void paper::bulk_pull_client::received_type ()
{
    auto this_l (shared_from_this ());
    paper::block_type type (static_cast <paper::block_type> (receive_buffer [0]));
    switch (type)
    {
        case paper::block_type::send:
        {
            boost::asio::async_read (connection->connection->socket, boost::asio::buffer (receive_buffer.data () + 1, paper::send_block::size), [this_l] (boost::system::error_code const & ec, size_t size_a)
            {
                this_l->received_block (ec, size_a);
            });
            break;
        }
        case paper::block_type::receive:
        {
            boost::asio::async_read (connection->connection->socket, boost::asio::buffer (receive_buffer.data () + 1, paper::receive_block::size), [this_l] (boost::system::error_code const & ec, size_t size_a)
            {
                this_l->received_block (ec, size_a);
            });
            break;
        }
        case paper::block_type::open:
        {
            boost::asio::async_read (connection->connection->socket, boost::asio::buffer (receive_buffer.data () + 1, paper::open_block::size), [this_l] (boost::system::error_code const & ec, size_t size_a)
            {
                this_l->received_block (ec, size_a);
            });
            break;
        }
        case paper::block_type::change:
        {
            boost::asio::async_read (connection->connection->socket, boost::asio::buffer (receive_buffer.data () + 1, paper::change_block::size), [this_l] (boost::system::error_code const & ec, size_t size_a)
            {
                this_l->received_block (ec, size_a);
            });
            break;
        }
        case paper::block_type::not_a_block:
        {
            request ();
            break;
        }
        default:
        {
            BOOST_LOG (connection->connection->node->log) << boost::str (boost::format ("Unknown type received as block type: %1%") % static_cast <int> (type));
            break;
        }
    }
}

paper::block_synchronization::block_synchronization (std::function <void (paper::block const &)> const & target_a, paper::block_store & store_a) :
target (target_a),
store (store_a)
{
}

paper::block_synchronization::~block_synchronization ()
{
}

namespace {
class add_dependency_visitor : public paper::block_visitor
{
public:
    add_dependency_visitor (paper::block_synchronization & sync_a) :
    sync (sync_a),
    result (true)
    {
    }
    void send_block (paper::send_block const & block_a) override
    {
        add_dependency (block_a.hashables.previous);
    }
    void receive_block (paper::receive_block const & block_a) override
    {
        add_dependency (block_a.hashables.previous);
        if (result)
        {
            add_dependency (block_a.hashables.source);
        }
    }
    void open_block (paper::open_block const & block_a) override
    {
        add_dependency (block_a.hashables.source);
    }
    void change_block (paper::change_block const & block_a) override
    {
        add_dependency (block_a.hashables.previous);
    }
    void add_dependency (paper::block_hash const & hash_a)
    {
        if (!sync.synchronized (hash_a))
        {
//			std::cerr << "Queueing: " << hash_a.to_string () << std::endl;
            result = false;
            sync.blocks.push (hash_a);
        }
		else
		{
//			std::cerr << "Completed: " << hash_a.to_string () << std::endl;
		}
    }
    paper::block_synchronization & sync;
    bool result;
};
}

bool paper::block_synchronization::add_dependency (paper::block const & block_a)
{
    add_dependency_visitor visitor (*this);
    block_a.visit (visitor);
    return visitor.result;
}

bool paper::block_synchronization::fill_dependencies ()
{
    auto result (false);
    auto done (false);
    while (!result && !done)
    {
		auto top (blocks.top ());
        auto block (retrieve (top));
        if (block != nullptr)
        {
            done = add_dependency (*block);
        }
        else
        {
            result = true;
        }
    }
    return result;
}

bool paper::block_synchronization::synchronize_one ()
{
    auto result (fill_dependencies ());
    if (!result)
    {
        auto block (retrieve (blocks.top ()));
        blocks.pop ();
        if (block != nullptr)
        {
			target (*block);
		}
		else
		{
			result = true;
		}
    }
    return result;
}

bool paper::block_synchronization::synchronize (paper::block_hash const & hash_a)
{
    auto result (false);
    blocks.push (hash_a);
    while (!result && !blocks.empty ())
    {
        result = synchronize_one ();
    }
    return result;
}

paper::pull_synchronization::pull_synchronization (std::function <void (paper::block const &)> const & target_a, paper::block_store & store_a) :
block_synchronization (target_a, store_a)
{
}

std::unique_ptr <paper::block> paper::pull_synchronization::retrieve (paper::block_hash const & hash_a)
{
	paper::transaction transaction (store.environment, nullptr, false);
    return store.unchecked_get (transaction, hash_a);
}

bool paper::pull_synchronization::synchronized (paper::block_hash const & hash_a)
{
	paper::transaction transaction (store.environment, nullptr, false);
    return store.block_exists (transaction, hash_a);
}

paper::push_synchronization::push_synchronization (std::function <void (paper::block const &)> const & target_a, paper::block_store & store_a) :
block_synchronization (target_a, store_a)
{
}

bool paper::push_synchronization::synchronized (paper::block_hash const & hash_a)
{
	paper::transaction transaction (store.environment, nullptr, true);
    auto result (!store.unsynced_exists (transaction, hash_a));
	if (!result)
	{
		store.unsynced_del (transaction, hash_a);
	}
	return result;
}

std::unique_ptr <paper::block> paper::push_synchronization::retrieve (paper::block_hash const & hash_a)
{
	paper::transaction transaction (store.environment, nullptr, false);
    return store.block_get (transaction, hash_a);
}

paper::block_hash paper::bulk_pull_client::first ()
{
	paper::block_hash result (0);
	paper::transaction transaction (connection->connection->node->store.environment, nullptr, false);
	auto iterator (connection->connection->node->store.unchecked_begin (transaction));
	if (iterator != connection->connection->node->store.unchecked_end ())
	{
		result = paper::block_hash (iterator->first);
	}
	return result;
}

void paper::bulk_pull_client::process_end ()
{
	paper::pull_synchronization synchronization ([this] (paper::block const & block_a)
	{
		auto process_result (connection->connection->node->process_receive (block_a));
		switch (process_result.code)
		{
			case paper::process_result::progress:
			case paper::process_result::old:
				break;
			case paper::process_result::fork:
				connection->connection->node->network.broadcast_confirm_req (block_a);
				BOOST_LOG (connection->connection->node->log) << boost::str (boost::format ("Fork received in bootstrap for block: %1%") % block_a.hash ().to_string ());
				break;
			default:
				BOOST_LOG (connection->connection->node->log) << boost::str (boost::format ("Error inserting block in bootstrap: %1%") % block_a.hash ().to_string ());
				break;
		}
		paper::transaction transaction (connection->connection->node->store.environment, nullptr, true);
		connection->connection->node->store.unchecked_del (transaction, block_a.hash ());
	}, connection->connection->node->store);
	paper::block_hash block (first ());
    while (!block.is_zero ())
    {
		auto error (synchronization.synchronize (block));
        if (error)
        {
			// Pulling account chains isn't transactional so updates happening during the process can cause dependency failures
			// A node may also have erroneously not sent the required dependent blocks
			BOOST_LOG (connection->connection->node->log) << boost::str (boost::format ("Error synchronizing block: %1%") % block.to_string ());
			paper::transaction transaction (connection->connection->node->store.environment, nullptr, true);
            while (!synchronization.blocks.empty ())
            {
                connection->connection->node->store.unchecked_del (transaction, synchronization.blocks.top ());
                synchronization.blocks.pop ();
            }
        }
		block = first ();
    }
}

void paper::bulk_pull_client::received_block (boost::system::error_code const & ec, size_t size_a)
{
	if (!ec)
	{
		paper::bufferstream stream (receive_buffer.data (), 1 + size_a);
		auto block (paper::deserialize_block (stream));
		if (block != nullptr)
		{
            auto hash (block->hash ());
            if (connection->connection->node->config.logging.bulk_pull_logging ())
            {
                std::string block_l;
                block->serialize_json (block_l);
                BOOST_LOG (connection->connection->node->log) << boost::str (boost::format ("Pulled block %1% %2%") % hash.to_string () % block_l);
            }
			paper::transaction transaction (connection->connection->node->store.environment, nullptr, true);
            connection->connection->node->store.unchecked_put (transaction, hash, *block);
            receive_block ();
		}
        else
        {
            BOOST_LOG (connection->connection->node->log) << "Error deserializing block received from pull request";
        }
	}
	else
	{
		BOOST_LOG (connection->connection->node->log) << boost::str (boost::format ("Error bulk receiving block: %1%") % ec.message ());
	}
}

paper::endpoint paper::network::endpoint ()
{
	boost::system::error_code ec;
	auto port (socket.local_endpoint (ec).port ());
	if (ec)
	{
		BOOST_LOG (node.log) << "Unable to retrieve port: " << ec.message ();
	}
    return paper::endpoint (boost::asio::ip::address_v6::loopback (), port);
}

boost::asio::ip::tcp::endpoint paper::bootstrap_listener::endpoint ()
{
    return boost::asio::ip::tcp::endpoint (boost::asio::ip::address_v6::loopback (), local.port ());
}

paper::bootstrap_server::~bootstrap_server ()
{
    if (node->config.logging.network_logging ())
    {
        BOOST_LOG (node->log) << "Exiting bootstrap server";
    }
}

void paper::peer_container::bootstrap_failed (paper::endpoint const & endpoint_a)
{
	std::lock_guard <std::mutex> lock (mutex);
	auto existing (peers.find (endpoint_a));
	if (existing != peers.end ())
	{
		peers.modify (existing, [] (paper::peer_information & info_a)
		{
			info_a.last_bootstrap_failure = std::chrono::system_clock::now ();
		});
	}
}

void paper::peer_container::random_fill (std::array <paper::endpoint, 8> & target_a)
{
    auto peers (list ());
    while (peers.size () > target_a.size ())
    {
        auto index (random_pool.GenerateWord32 (0, peers.size () - 1));
        assert (index < peers.size ());
		assert (index >= 0);
		if (index != peers.size () - 1)
		{
				peers [index] = peers [peers.size () - 1];
		}
        peers.pop_back ();
    }
    assert (peers.size () <= target_a.size ());
    auto endpoint (paper::endpoint (boost::asio::ip::address_v6 {}, 0));
    assert (endpoint.address ().is_v6 ());
    std::fill (target_a.begin (), target_a.end (), endpoint);
    auto j (target_a.begin ());
    for (auto i (peers.begin ()), n (peers.end ()); i != n; ++i, ++j)
    {
        assert (i->endpoint.address ().is_v6 ());
        assert (j < target_a.end ());
        *j = i->endpoint;
    }
}

std::vector <paper::peer_information> paper::peer_container::purge_list (std::chrono::system_clock::time_point const & cutoff)
{
	std::vector <paper::peer_information> result;
	{
		std::lock_guard <std::mutex> lock (mutex);
		auto pivot (peers.get <1> ().lower_bound (cutoff));
		result.assign (pivot, peers.get <1> ().end ());
		peers.get <1> ().erase (peers.get <1> ().begin (), pivot);
		for (auto i (peers.begin ()), n (peers.end ()); i != n; ++i)
		{
			peers.modify (i, [] (paper::peer_information & info) {info.last_attempt = std::chrono::system_clock::now ();});
		}
	}
	if (result.empty ())
	{
		disconnect_observer ();
	}
    return result;
}

size_t paper::peer_container::size ()
{
    std::lock_guard <std::mutex> lock (mutex);
    return peers.size ();
}

bool paper::peer_container::empty ()
{
    return size () == 0;
}

bool paper::peer_container::not_a_peer (paper::endpoint const & endpoint_a)
{
    bool result (false);
    if (endpoint_a.address ().to_v6 ().is_unspecified ())
    {
        result = true;
    }
    else if (paper::reserved_address (endpoint_a))
    {
        result = true;
    }
    else if (endpoint_a == self)
    {
        result = true;
    }
    return result;
}

bool paper::peer_container::insert (paper::endpoint const & endpoint_a)
{
    return insert (endpoint_a, paper::block_hash (0));
}

bool paper::peer_container::knows_about (paper::endpoint const & endpoint_a, paper::block_hash const & hash_a)
{
    std::lock_guard <std::mutex> lock (mutex);
    bool result (false);
    auto existing (peers.find (endpoint_a));
    if (existing != peers.end ())
    {
        result = existing->most_recent == hash_a;
    }
    return result;
}

bool paper::peer_container::insert (paper::endpoint const & endpoint_a, paper::block_hash const & hash_a)
{
	auto unknown (false);
    auto result (not_a_peer (endpoint_a));
    if (!result)
    {
        std::lock_guard <std::mutex> lock (mutex);
        auto existing (peers.find (endpoint_a));
        if (existing != peers.end ())
        {
            peers.modify (existing, [&hash_a] (paper::peer_information & info)
            {
                info.last_contact = std::chrono::system_clock::now ();
                info.most_recent = hash_a;
            });
            result = true;
        }
        else
        {
            peers.insert ({endpoint_a, std::chrono::system_clock::now (), std::chrono::system_clock::now (), std::chrono::system_clock::time_point (), hash_a});
			unknown = true;
        }
    }
	if (unknown)
	{
		peer_observer (endpoint_a);
	}
    return result;
}

namespace {
boost::asio::ip::address_v6 mapped_from_v4_bytes (unsigned long address_a)
{
    return boost::asio::ip::address_v6::v4_mapped (boost::asio::ip::address_v4 (address_a));
}
}

bool paper::reserved_address (paper::endpoint const & endpoint_a)
{
    assert (endpoint_a.address ().is_v6 ());
	auto bytes (endpoint_a.address ().to_v6 ());
	auto result (false);
    if (bytes >= mapped_from_v4_bytes (0x00000000ul) && bytes <= mapped_from_v4_bytes (0x00fffffful)) // Broadcast RFC1700
	{
		result = true;
	}
	else if (bytes >= mapped_from_v4_bytes (0xc0000200ul) && bytes <= mapped_from_v4_bytes (0xc00002fful)) // TEST-NET RFC5737
	{
		result = true;
	}
	else if (bytes >= mapped_from_v4_bytes (0xc6336400ul) && bytes <= mapped_from_v4_bytes (0xc63364fful)) // TEST-NET-2 RFC5737
	{
		result = true;
	}
	else if (bytes >= mapped_from_v4_bytes (0xcb007100ul) && bytes <= mapped_from_v4_bytes (0xcb0071fful)) // TEST-NET-3 RFC5737
	{
		result = true;
	}
	else if (bytes >= mapped_from_v4_bytes (0xe9fc0000ul) && bytes <= mapped_from_v4_bytes (0xe9fc00fful))
	{
		result = true;
	}
	else if (bytes >= mapped_from_v4_bytes (0xf0000000ul)) // Reserved RFC6890
	{
		result = true;
	}
	return result;
}

paper::peer_container::peer_container (paper::endpoint const & self_a) :
self (self_a),
peer_observer ([] (paper::endpoint const &) {}),
disconnect_observer ([] () {})
{
}

void paper::peer_container::contacted (paper::endpoint const & endpoint_a)
{
    auto endpoint_l (endpoint_a);
    if (endpoint_l.address ().is_v4 ())
    {
        endpoint_l = paper::endpoint (boost::asio::ip::address_v6::v4_mapped (endpoint_l.address ().to_v4 ()), endpoint_l.port ());
    }
    assert (endpoint_l.address ().is_v6 ());
	insert (endpoint_l);
}

std::ostream & operator << (std::ostream & stream_a, std::chrono::system_clock::time_point const & time_a)
{
    time_t last_contact (std::chrono::system_clock::to_time_t (time_a));
    std::string string (ctime (&last_contact));
    string.pop_back ();
    stream_a << string;
    return stream_a;
}

void paper::network::initiate_send ()
{
	assert (!socket_mutex.try_lock ());
	assert (!sends.empty ());
	auto & front (sends.front ());
	if (node.config.logging.network_packet_logging ())
	{
		BOOST_LOG (node.log) << "Sending packet";
	}
	socket.async_send_to (boost::asio::buffer (front.data, front.size), front.endpoint, [this, front] (boost::system::error_code const & ec, size_t size_a)
	{
		if (front.rebroadcast > 0)
		{
			node.service.add (std::chrono::system_clock::now () + std::chrono::seconds (node.config.rebroadcast_delay), [this, front]
			{
				send_buffer (front.data, front.size, front.endpoint, front.rebroadcast - 1, front.callback);
			});
		}
		else
		{
			paper::send_info self;
			{
				std::unique_lock <std::mutex> lock (socket_mutex);
				assert (!sends.empty ());
				self = sends.front ();
			}
			self.callback (ec, size_a);
		}
		send_complete (ec, size_a);
	});
}

void paper::network::send_buffer (uint8_t const * data_a, size_t size_a, paper::endpoint const & endpoint_a, size_t rebroadcast_a, std::function <void (boost::system::error_code const &, size_t)> callback_a)
{
	std::unique_lock <std::mutex> lock (socket_mutex);
	auto initiate (sends.empty ());
	sends.push ({data_a, size_a, endpoint_a, rebroadcast_a, callback_a});
	if (initiate)
	{
		initiate_send ();
	}
}

void paper::network::send_complete (boost::system::error_code const & ec, size_t size_a)
{
    if (node.config.logging.network_packet_logging ())
    {
        BOOST_LOG (node.log) << "Packet send complete";
    }
	std::unique_lock <std::mutex> lock (socket_mutex);
	assert (!sends.empty ());
	sends.pop ();
	if (!sends.empty ())
	{
		if (node.config.logging.network_packet_logging ())
		{
			BOOST_LOG (node.log) << boost::str (boost::format ("Delaying next packet send %1% microseconds") % node.config.packet_delay_microseconds);
		}
		node.service.add (std::chrono::system_clock::now () + std::chrono::microseconds (node.config.packet_delay_microseconds), [this] ()
		{
			std::unique_lock <std::mutex> lock (socket_mutex);
			initiate_send ();
		});
	}
}

uint64_t paper::block_store::now ()
{
    boost::posix_time::ptime epoch (boost::gregorian::date (1970, 1, 1));
    auto now (boost::posix_time::second_clock::universal_time ());
    auto diff (now - epoch);
    return diff.total_seconds ();
}

paper::bulk_push_server::bulk_push_server (std::shared_ptr <paper::bootstrap_server> const & connection_a) :
connection (connection_a)
{
}

void paper::bulk_push_server::receive ()
{
    auto this_l (shared_from_this ());
    boost::asio::async_read (*connection->socket, boost::asio::buffer (receive_buffer.data (), 1), [this_l] (boost::system::error_code const & ec, size_t size_a)
        {
            if (!ec)
            {
                this_l->received_type ();
            }
            else
            {
                BOOST_LOG (this_l->connection->node->log) << boost::str (boost::format ("Error receiving block type %1%") % ec.message ());
            }
        });
}

void paper::bulk_push_server::received_type ()
{
    auto this_l (shared_from_this ());
    paper::block_type type (static_cast <paper::block_type> (receive_buffer [0]));
    switch (type)
    {
        case paper::block_type::send:
        {
            boost::asio::async_read (*connection->socket, boost::asio::buffer (receive_buffer.data () + 1, paper::send_block::size), [this_l] (boost::system::error_code const & ec, size_t size_a)
                                     {
                                         this_l->received_block (ec, size_a);
                                     });
            break;
        }
        case paper::block_type::receive:
        {
            boost::asio::async_read (*connection->socket, boost::asio::buffer (receive_buffer.data () + 1, paper::receive_block::size), [this_l] (boost::system::error_code const & ec, size_t size_a)
                                     {
                                         this_l->received_block (ec, size_a);
                                     });
            break;
        }
        case paper::block_type::open:
        {
            boost::asio::async_read (*connection->socket, boost::asio::buffer (receive_buffer.data () + 1, paper::open_block::size), [this_l] (boost::system::error_code const & ec, size_t size_a)
                                     {
                                         this_l->received_block (ec, size_a);
                                     });
            break;
        }
        case paper::block_type::change:
        {
            boost::asio::async_read (*connection->socket, boost::asio::buffer (receive_buffer.data () + 1, paper::change_block::size), [this_l] (boost::system::error_code const & ec, size_t size_a)
                                     {
                                         this_l->received_block (ec, size_a);
                                     });
            break;
        }
        case paper::block_type::not_a_block:
        {
            connection->finish_request ();
            break;
        }
        default:
        {
            BOOST_LOG (connection->node->log) << "Unknown type received as block type";
            break;
        }
    }
}

void paper::bulk_push_server::received_block (boost::system::error_code const & ec, size_t size_a)
{
    if (!ec)
    {
        paper::bufferstream stream (receive_buffer.data (), 1 + size_a);
        auto block (paper::deserialize_block (stream));
        if (block != nullptr)
        {
            connection->node->process_receive_republish (std::move (block), 0);
            receive ();
        }
        else
        {
            BOOST_LOG (connection->node->log) << "Error deserializing block received from pull request";
        }
    }
}

paper::bulk_pull_server::bulk_pull_server (std::shared_ptr <paper::bootstrap_server> const & connection_a, std::unique_ptr <paper::bulk_pull> request_a) :
connection (connection_a),
request (std::move (request_a))
{
    set_current_end ();
}

paper::frontier_req_server::frontier_req_server (std::shared_ptr <paper::bootstrap_server> const & connection_a, std::unique_ptr <paper::frontier_req> request_a) :
connection (connection_a),
current (request_a->start.number () - 1),
info (0, 0, 0, 0, false),
request (std::move (request_a))
{
	next ();
    skip_old ();
}

void paper::frontier_req_server::skip_old ()
{
    if (request->age != std::numeric_limits<decltype (request->age)>::max ())
    {
        auto now (connection->node->store.now ());
        while (!current.is_zero () && (now - info.modified) >= request->age)
        {
            next ();
        }
    }
}

void paper::frontier_req_server::send_next ()
{
    if (!current.is_zero ())
    {
        {
            send_buffer.clear ();
            paper::vectorstream stream (send_buffer);
            write (stream, current.bytes);
            write (stream, info.head.bytes);
        }
        auto this_l (shared_from_this ());
        if (connection->node->config.logging.network_logging ())
        {
            BOOST_LOG (connection->node->log) << boost::str (boost::format ("Sending frontier for %1% %2%") % current.to_base58check () % info.head.to_string ());
        }
		next ();
        async_write (*connection->socket, boost::asio::buffer (send_buffer.data (), send_buffer.size ()), [this_l] (boost::system::error_code const & ec, size_t size_a)
        {
            this_l->sent_action (ec, size_a);
        });
    }
    else
    {
        send_finished ();
    }
}

void paper::frontier_req_server::send_finished ()
{
    {
        send_buffer.clear ();
        paper::vectorstream stream (send_buffer);
        paper::uint256_union zero (0);
        write (stream, zero.bytes);
        write (stream, zero.bytes);
    }
    auto this_l (shared_from_this ());
    if (connection->node->config.logging.network_logging ())
    {
        BOOST_LOG (connection->node->log) << "Frontier sending finished";
    }
    async_write (*connection->socket, boost::asio::buffer (send_buffer.data (), send_buffer.size ()), [this_l] (boost::system::error_code const & ec, size_t size_a)
    {
        this_l->no_block_sent (ec, size_a);
    });
}

void paper::frontier_req_server::no_block_sent (boost::system::error_code const & ec, size_t size_a)
{
    if (!ec)
    {
		connection->finish_request ();
    }
    else
    {
        if (connection->node->config.logging.network_logging ())
        {
            BOOST_LOG (connection->node->log) << boost::str (boost::format ("Error sending frontier finish %1%") % ec.message ());
        }
    }
}

void paper::frontier_req_server::sent_action (boost::system::error_code const & ec, size_t size_a)
{
    if (!ec)
    {
        send_next ();
    }
    else
    {
        if (connection->node->config.logging.network_logging ())
        {
            BOOST_LOG (connection->node->log) << boost::str (boost::format ("Error sending frontier pair %1%") % ec.message ());
        }
    }
}

void paper::frontier_req_server::next ()
{
	paper::transaction transaction (connection->node->store.environment, nullptr, false);
	auto iterator (connection->node->store.latest_begin (transaction, current.number () + 1));
	if (iterator != connection->node->store.latest_end ())
	{
		current = paper::uint256_union (iterator->first);
		info = paper::account_info (iterator->second);
	}
	else
	{
		current.clear ();
	}
}

paper::frontier_req::frontier_req () :
message (paper::message_type::frontier_req)
{
}

bool paper::frontier_req::deserialize (paper::stream & stream_a)
{
	auto result (read_header (stream_a, version_max, version_using, version_min, type, extensions));
	assert (!result);
	assert (paper::message_type::frontier_req == type);
    if (!result)
    {
        assert (type == paper::message_type::frontier_req);
        result = read (stream_a, start.bytes);
        if (!result)
        {
            result = read (stream_a, age);
            if (!result)
            {
                result = read (stream_a, count);
            }
        }
    }
    return result;
}

void paper::frontier_req::serialize (paper::stream & stream_a)
{
	write_header (stream_a);
    write (stream_a, start.bytes);
    write (stream_a, age);
    write (stream_a, count);
}

void paper::frontier_req::visit (paper::message_visitor & visitor_a) const
{
    visitor_a.frontier_req (*this);
}

bool paper::frontier_req::operator == (paper::frontier_req const & other_a) const
{
    return start == other_a.start && age == other_a.age && count == other_a.count;
}

paper::bulk_pull_client::bulk_pull_client (std::shared_ptr <paper::frontier_req_client> const & connection_a) :
connection (connection_a),
current (connection->pulls.begin ()),
end (connection->pulls.end ())
{
}

paper::bulk_pull_client::~bulk_pull_client ()
{
    if (connection->connection->node->config.logging.network_logging ())
    {
        BOOST_LOG (connection->connection->node->log) << "Exiting bulk pull client";
    }
}

paper::frontier_req_client::frontier_req_client (std::shared_ptr <paper::bootstrap_client> const & connection_a) :
connection (connection_a),
current (0)
{
	next ();
}

paper::frontier_req_client::~frontier_req_client ()
{
    if (connection->node->config.logging.network_logging ())
    {
        BOOST_LOG (connection->node->log) << "Exiting frontier_req initiator";
    }
}

void paper::frontier_req_client::receive_frontier ()
{
    auto this_l (shared_from_this ());
    boost::asio::async_read (connection->socket, boost::asio::buffer (receive_buffer.data (), sizeof (paper::uint256_union) + sizeof (paper::uint256_union)), [this_l] (boost::system::error_code const & ec, size_t size_a)
    {
        this_l->received_frontier (ec, size_a);
    });
}

void paper::frontier_req_client::request_account (paper::account const & account_a)
{
    // Account they know about and we don't.
    pulls [account_a] = paper::block_hash (0);
}

void paper::frontier_req_client::completed_pulls ()
{
    auto this_l (shared_from_this ());
    auto pushes (std::make_shared <paper::bulk_push_client> (this_l));
    pushes->start ();
}

void paper::frontier_req_client::unsynced (MDB_txn * transaction_a, paper::block_hash const & ours_a, paper::block_hash const & theirs_a)
{
	auto current (ours_a);
	while (!current.is_zero () && current != theirs_a)
	{
		connection->node->store.unsynced_put (transaction_a, current);
		auto block (connection->node->store.block_get (transaction_a, current));
		current = block->previous ();
	}
}

void paper::frontier_req_client::received_frontier (boost::system::error_code const & ec, size_t size_a)
{
    if (!ec)
    {
        assert (size_a == sizeof (paper::uint256_union) + sizeof (paper::uint256_union));
        paper::account account;
        paper::bufferstream account_stream (receive_buffer.data (), sizeof (paper::uint256_union));
        auto error1 (paper::read (account_stream, account));
        assert (!error1);
        paper::block_hash latest;
        paper::bufferstream latest_stream (receive_buffer.data () + sizeof (paper::uint256_union), sizeof (paper::uint256_union));
        auto error2 (paper::read (latest_stream, latest));
        assert (!error2);
        if (!account.is_zero ())
        {
            while (!current.is_zero () && current < account)
            {
				paper::transaction transaction (connection->node->store.environment, nullptr, true);
                // We know about an account they don't.
				unsynced (transaction, info.head, 0);
				next ();
            }
            if (!current.is_zero ())
            {
                if (account == current)
                {
                    if (latest == info.head)
                    {
                        // In sync
                    }
                    else
					{
						paper::transaction transaction (connection->node->store.environment, nullptr, true);
						if (connection->node->store.block_exists (transaction, latest))
						{
							// We know about a block they don't.
							unsynced (transaction, info.head, latest);
						}
						else
						{
							// They know about a block we don't.
							pulls [account] = info.head;
						}
					}
					next ();
                }
                else
                {
                    assert (account < current);
                    request_account (account);
                }
            }
            else
            {
                request_account (account);
            }
            receive_frontier ();
        }
        else
        {
			{
				paper::transaction transaction (connection->node->store.environment, nullptr, true);
				while (!current.is_zero ())
				{
					// We know about an account they don't.
					unsynced (transaction, info.head, 0);
					next ();
				}
			}
            completed_requests ();
        }
    }
    else
    {
        if (connection->node->config.logging.network_logging ())
        {
            BOOST_LOG (connection->node->log) << boost::str (boost::format ("Error while receiving frontier %1%") % ec.message ());
        }
    }
}

void paper::frontier_req_client::next ()
{
	paper::transaction transaction (connection->node->store.environment, nullptr, false);
	auto iterator (connection->node->store.latest_begin (transaction, paper::uint256_union (current.number () + 1)));
	if (iterator != connection->node->store.latest_end ())
	{
		current = paper::account (iterator->first);
		info = paper::account_info (iterator->second);
	}
	else
	{
		current.clear ();
	}
}

void paper::frontier_req_client::completed_requests ()
{
    auto this_l (shared_from_this ());
    auto pulls (std::make_shared <paper::bulk_pull_client> (this_l));
    pulls->request ();
}

void paper::frontier_req_client::completed_pushes ()
{
}

paper::bulk_push_client::bulk_push_client (std::shared_ptr <paper::frontier_req_client> const & connection_a) :
connection (connection_a),
synchronization ([this] (paper::block const & block_a)
{
    push_block (block_a);
}, connection_a->connection->node->store)
{
}

paper::bulk_push_client::~bulk_push_client ()
{
    if (connection->connection->node->config.logging.network_logging ())
    {
        BOOST_LOG (connection->connection->node->log) << "Exiting bulk push client";
    }
}

void paper::bulk_push_client::start ()
{
    paper::bulk_push message;
    auto buffer (std::make_shared <std::vector <uint8_t>> ());
    {
        paper::vectorstream stream (*buffer);
        message.serialize (stream);
    }
    auto this_l (shared_from_this ());
    boost::asio::async_write (connection->connection->socket, boost::asio::buffer (buffer->data (), buffer->size ()), [this_l, buffer] (boost::system::error_code const & ec, size_t size_a)
        {
            if (!ec)
            {
                this_l->push ();
            }
            else
            {
                BOOST_LOG (this_l->connection->connection->node->log) << boost::str (boost::format ("Unable to send bulk_push request %1%") % ec.message ());
            }
        });
}

void paper::bulk_push_client::push ()
{
	paper::block_hash hash (0);
	{
		paper::transaction transaction (connection->connection->node->store.environment, nullptr, true);
		auto first (connection->connection->node->store.unsynced_begin (transaction));
		if (first != paper::store_iterator (nullptr))
		{
			hash = first->first;
			connection->connection->node->store.unsynced_del (transaction, hash);
		}
	}
	if (!hash.is_zero ())
	{
		synchronization.blocks.push (hash);
        synchronization.synchronize_one ();
    }
    else
    {
        send_finished ();
    }
}

void paper::bulk_push_client::send_finished ()
{
    auto buffer (std::make_shared <std::vector <uint8_t>> ());
    buffer->push_back (static_cast <uint8_t> (paper::block_type::not_a_block));
    if (connection->connection->node->config.logging.network_logging ())
    {
        BOOST_LOG (connection->connection->node->log) << "Bulk push finished";
    }
    auto this_l (shared_from_this ());
    async_write (connection->connection->socket, boost::asio::buffer (buffer->data (), 1), [this_l] (boost::system::error_code const & ec, size_t size_a)
        {
            this_l->connection->completed_pushes ();
        });
}

void paper::bulk_push_client::push_block (paper::block const & block_a)
{
    auto buffer (std::make_shared <std::vector <uint8_t>> ());
    {
        paper::vectorstream stream (*buffer);
        paper::serialize_block (stream, block_a);
    }
    auto this_l (shared_from_this ());
    boost::asio::async_write (connection->connection->socket, boost::asio::buffer (buffer->data (), buffer->size ()), [this_l, buffer] (boost::system::error_code const & ec, size_t size_a)
	{
		if (!ec)
		{
			if (!this_l->synchronization.blocks.empty ())
			{
				this_l->synchronization.synchronize_one ();
			}
			else
			{
				this_l->push ();
			}
		}
		else
		{
			BOOST_LOG (this_l->connection->connection->node->log) << boost::str (boost::format ("Error sending block during bulk push %1%") % ec.message ());
		}
	});
}

bool paper::keepalive::operator == (paper::keepalive const & other_a) const
{
	return peers == other_a.peers;
}

bool paper::peer_container::known_peer (paper::endpoint const & endpoint_a)
{
    std::lock_guard <std::mutex> lock (mutex);
    auto existing (peers.find (endpoint_a));
    return existing != peers.end () && existing->last_contact > std::chrono::system_clock::now () - paper::node::cutoff;
}

std::shared_ptr <paper::node> paper::node::shared ()
{
    return shared_from_this ();
}

namespace
{
class traffic_generator : public std::enable_shared_from_this <traffic_generator>
{
public:
    traffic_generator (uint32_t count_a, uint32_t wait_a, std::shared_ptr <paper::node> node_a, paper::system & system_a) :
    count (count_a),
    wait (wait_a),
    node (node_a),
    system (system_a)
    {
    }
    void run ()
    {
        auto count_l (count - 1);
        count = count_l - 1;
        system.generate_activity (*node);
        if (count_l > 0)
        {
            auto this_l (shared_from_this ());
            node->service.add (std::chrono::system_clock::now () + std::chrono::milliseconds (wait), [this_l] () {this_l->run ();});
        }
    }
    uint32_t count;
    uint32_t wait;
    std::shared_ptr <paper::node> node;
    paper::system & system;
};
}

void paper::system::generate_usage_traffic (uint32_t count_a, uint32_t wait_a)
{
    for (size_t i (0), n (nodes.size ()); i != n; ++i)
    {
        generate_usage_traffic (count_a, wait_a, i);
    }
}

void paper::system::generate_usage_traffic (uint32_t count_a, uint32_t wait_a, size_t index_a)
{
    assert (nodes.size () > index_a);
    assert (count_a > 0);
    auto generate (std::make_shared <traffic_generator> (count_a, wait_a, nodes [index_a], *this));
    generate->run ();
}

void paper::system::generate_activity (paper::node & node_a)
{
    auto what (random_pool.GenerateByte ());
    if (what < 0xc0)
    {
        generate_send_existing (node_a);
    }
    else
    {
        generate_send_new (node_a);
    }
}

paper::account paper::system::get_random_account (MDB_txn * transaction_a, paper::node & node_a)
{
	auto accounts (wallet (0)->store.accounts (transaction_a));
	auto index (random_pool.GenerateWord32 (0, accounts.size () - 1));
	auto result (accounts [index]);
	return result;
}

paper::uint128_t paper::system::get_random_amount (MDB_txn * transaction_a, paper::node & node_a, paper::account const & account_a)
{
    paper::uint128_t balance (node_a.ledger.account_balance (transaction_a, account_a));
    std::string balance_text (balance.convert_to <std::string> ());
    paper::uint128_union random_amount;
    random_pool.GenerateBlock (random_amount.bytes.data (), sizeof (random_amount.bytes));
    auto result (((paper::uint256_t {random_amount.number ()} * balance) / paper::uint256_t {std::numeric_limits <paper::uint128_t>::max ()}).convert_to <paper::uint128_t> ());
    std::string text (result.convert_to <std::string> ());
    return result;
}

void paper::system::generate_send_existing (paper::node & node_a)
{
	paper::uint128_t amount;
	paper::account destination;
	paper::account source;
	{
		paper::account account;
		random_pool.GenerateBlock (account.bytes.data (), sizeof (account.bytes));
		paper::transaction transaction (node_a.store.environment, nullptr, false);
		paper::store_iterator entry (node_a.store.latest_begin (transaction, account));
		if (entry == node_a.store.latest_end ())
		{
			entry = node_a.store.latest_begin (transaction);
		}
		assert (entry != node_a.store.latest_end ());
		destination = paper::account (entry->first);
		source = get_random_account (transaction, node_a);
		amount = get_random_amount (transaction, node_a, source);
	}
    wallet (0)->send_sync (source, destination, amount);
}

void paper::system::generate_send_new (paper::node & node_a)
{
    assert (node_a.wallets.items.size () == 1);
    paper::keypair key;
	paper::uint128_t amount;
	paper::account source;
	{
		paper::transaction transaction (node_a.store.environment, nullptr, false);
		source = get_random_account (transaction, node_a);
		amount = get_random_amount (transaction, node_a, source);
	}
	node_a.wallets.items.begin ()->second->insert (key.prv);
    node_a.wallets.items.begin ()->second->send_sync (source, key.pub, amount);
}

void paper::system::generate_mass_activity (uint32_t count_a, paper::node & node_a)
{
    auto previous (std::chrono::system_clock::now ());
    for (uint32_t i (0); i < count_a; ++i)
    {
        if ((i & 0x3ff) == 0)
        {
            auto now (std::chrono::system_clock::now ());
            auto ms (std::chrono::duration_cast <std::chrono::milliseconds> (now - previous).count ());
            std::cerr << boost::str (boost::format ("Mass activity iteration %1% ms %2% ms/t %3%\n") % i % ms % (ms / 256));
            previous = now;
        }
        generate_activity (node_a);
    }
}

paper::election::election (std::shared_ptr <paper::node> node_a, paper::block const & block_a, std::function <void (paper::block &)> const & confirmation_action_a) :
votes (block_a.root ()),
node (node_a),
last_vote (std::chrono::system_clock::now ()),
last_winner (block_a.clone ()),
confirmed (false),
confirmation_action (confirmation_action_a)
{
	{
		paper::transaction transaction (node_a->store.environment, nullptr, false);
		assert (node_a->store.block_exists (transaction, block_a.hash ()));
	}
    paper::keypair anonymous;
    paper::vote vote_l (anonymous.pub, anonymous.prv, 0, block_a.clone ());
    vote (vote_l);
}

void paper::election::start ()
{
	auto node_l (node.lock ());
	if (node_l != nullptr)
	{
		auto have_representative (node_l->representative_vote (*this, *last_winner));
		if (have_representative)
		{
			announce_vote ();
		}
		timeout_action ();
	}
}

void paper::election::timeout_action ()
{
	auto node_l (node.lock ());
	if (node_l != nullptr)
	{
		auto now (std::chrono::system_clock::now ());
		if (now - last_vote < std::chrono::seconds (15))
		{
			auto this_l (shared_from_this ());
			node_l->service.add (now + std::chrono::seconds (15), [this_l] () {this_l->timeout_action ();});
		}
		else
		{
			auto root_l (votes.id);
			node_l->conflicts.stop (root_l);
			if (!confirmed)
			{
				BOOST_LOG (node_l->log) << boost::str (boost::format ("Election timed out for block %1%") % last_winner->hash ().to_string ());
			}
		}
	}
}

paper::uint128_t paper::election::uncontested_threshold (MDB_txn * transaction_a, paper::ledger & ledger_a)
{
    return ledger_a.supply (transaction_a) / 2;
}

paper::uint128_t paper::election::contested_threshold (MDB_txn * transaction_a, paper::ledger & ledger_a)
{
    return (ledger_a.supply (transaction_a) / 16) * 15;
}

void paper::election::vote (paper::vote const & vote_a)
{
	auto node_l (node.lock ());
	if (node_l != nullptr)
	{
		auto changed (votes.vote (vote_a));
		std::unique_ptr <paper::block> winner;
		auto was_confirmed (confirmed);
		{
			paper::transaction transaction (node_l->store.environment, nullptr, true);
			if (!was_confirmed && changed)
			{
				auto tally_l (node_l->ledger.tally (transaction, votes));
				assert (tally_l.size () > 0);
				winner = tally_l.begin ()->second->clone ();
				if (!(*winner == *last_winner))
				{
					node_l->ledger.rollback (transaction, last_winner->hash ());
					node_l->ledger.process (transaction, *winner);
					last_winner = winner->clone ();
				}
				if (tally_l.size () == 1)
				{
					if (tally_l.begin ()->first > uncontested_threshold (transaction, node_l->ledger))
					{
						confirmed = true;
					}
				}
				else
				{
					if (tally_l.begin ()->first > contested_threshold (transaction, node_l->ledger))
					{
						confirmed = true;
					}
				}
			}
		}
		if (!was_confirmed && confirmed)
		{
			std::shared_ptr <paper::block> winner_l (winner.release ());
			auto confirmation_action_l (confirmation_action);
			node_l->service.add (std::chrono::system_clock::now (), [winner_l, confirmation_action_l] ()
			{
				confirmation_action_l (*winner_l);
			});
		}
	}
}

void paper::election::start_request (paper::block const & block_a)
{
	auto node_l (node.lock ());
	if (node_l != nullptr)
	{
		node_l->network.broadcast_confirm_req (block_a);
	}
}

void paper::election::announce_vote ()
{
	auto node_l (node.lock ());
	if (node_l != nullptr)
	{
		std::pair <paper::uint128_t, std::unique_ptr <paper::block>> winner_l;
		{
			paper::transaction transaction (node_l->store.environment, nullptr, false);
			winner_l = node_l->ledger.winner (transaction, votes);
		}
		assert (winner_l.second != nullptr);
		auto list (node_l->peers.list ());
		node_l->network.confirm_broadcast (list, std::move (winner_l.second), votes.sequence, 0);
		auto now (std::chrono::system_clock::now ());
		if (now - last_vote < std::chrono::seconds (15))
		{
			auto this_l (shared_from_this ());
			node_l->service.add (now + std::chrono::seconds (15), [this_l] () {this_l->announce_vote ();});
		}
	}
}

void paper::conflicts::start (paper::block const & block_a, std::function <void (paper::block &)> const & confirmation_action_a, bool request_a)
{
    std::lock_guard <std::mutex> lock (mutex);
    auto root (block_a.root ());
    auto existing (roots.find (root));
    if (existing == roots.end ())
    {
        auto election (std::make_shared <paper::election> (node.shared (), block_a, confirmation_action_a));
		node.service.add (std::chrono::system_clock::now (), [election] () {election->start ();});
        roots.insert (std::make_pair (root, election));
        if (request_a)
        {
            election->start_request (block_a);
        }
    }
}

bool paper::conflicts::no_conflict (paper::block_hash const & hash_a)
{
    std::lock_guard <std::mutex> lock (mutex);
    auto result (true);
    auto existing (roots.find (hash_a));
    if (existing != roots.end ())
    {
        auto size (existing->second->votes.rep_votes.size ());
		if (size > 1)
		{
			auto & block (existing->second->votes.rep_votes.begin ()->second.second);
			for (auto i (existing->second->votes.rep_votes.begin ()), n (existing->second->votes.rep_votes.end ()); i != n && result; ++i)
			{
				result = *block == *i->second.second;
			}
		}
    }
    return result;
}

// Validate a vote and apply it to the current election or start a new election if it doesn't exist
void paper::conflicts::update (paper::vote const & vote_a)
{
    std::lock_guard <std::mutex> lock (mutex);
    auto existing (roots.find (vote_a.block->root ()));
    if (existing != roots.end ())
    {
        existing->second->vote (vote_a);
    }
}

void paper::conflicts::stop (paper::block_hash const & root_a)
{
    std::lock_guard <std::mutex> lock (mutex);
    assert (roots.find (root_a) != roots.end ());
    roots.erase (root_a);
}

paper::conflicts::conflicts (paper::node & node_a) :
node (node_a)
{
}

bool paper::node::representative_vote (paper::election & election_a, paper::block const & block_a)
{
    bool result (false);
	for (auto i (wallets.items.begin ()), n (wallets.items.end ()); i != n; ++i)
	{
		auto is_representative (false);
		paper::vote vote_l;
		{
			paper::transaction transaction (store.environment, nullptr, false);
			is_representative = i->second->store.is_representative (transaction);
			if (is_representative)
			{
				auto representative (i->second->store.representative (transaction));
				paper::private_key prv;
				auto error (i->second->store.fetch (transaction, representative, prv));
				(void)error;
				vote_l = paper::vote (representative, prv, 0, block_a.clone ());
				prv.clear ();
				result = true;
			}
		}
		if (is_representative)
		{
			election_a.vote (vote_l);
		}
	}
    return result;
}

paper::uint256_union paper::wallet_store::check (MDB_txn * transaction_a)
{
	paper::wallet_value value (entry_get_raw (transaction_a, paper::wallet_store::check_special));
    return value.key;
}

paper::uint256_union paper::wallet_store::salt (MDB_txn * transaction_a)
{
	paper::wallet_value value (entry_get_raw (transaction_a, paper::wallet_store::salt_special));
    return value.key;
}

paper::uint256_union paper::wallet_store::wallet_key (MDB_txn * transaction_a)
{
	paper::wallet_value value (entry_get_raw (transaction_a, paper::wallet_store::wallet_key_special));
    auto password_l (password.value ());
    auto result (value.key.prv (password_l, salt (transaction_a).owords [0]));
    password_l.clear ();
    return result;
}

bool paper::wallet_store::valid_password (MDB_txn * transaction_a)
{
    paper::uint256_union zero;
    zero.clear ();
    auto wallet_key_l (wallet_key (transaction_a));
    paper::uint256_union check_l (zero, wallet_key_l, salt (transaction_a).owords [0]);
    wallet_key_l.clear ();
    return check (transaction_a) == check_l;
}

void paper::wallet_store::enter_password (MDB_txn * transaction_a, std::string const & password_a)
{
    password.value_set (derive_key (transaction_a, password_a));
}

bool paper::wallet_store::rekey (MDB_txn * transaction_a, std::string const & password_a)
{
    bool result (false);
	if (valid_password (transaction_a))
    {
        auto password_new (derive_key (transaction_a, password_a));
        auto wallet_key_l (wallet_key (transaction_a));
        auto password_l (password.value ());
        (*password.values [0]) ^= password_l;
        (*password.values [0]) ^= password_new;
        paper::uint256_union encrypted (wallet_key_l, password_new, salt (transaction_a).owords [0]);
		entry_put_raw (transaction_a, paper::wallet_store::wallet_key_special, paper::wallet_value (encrypted));
        wallet_key_l.clear ();
    }
    else
    {
        result = true;
    }
    return result;
}

paper::uint256_union paper::wallet_store::derive_key (MDB_txn * transaction_a, std::string const & password_a)
{
    paper::kdf kdf (kdf_work);
    auto result (kdf.generate (password_a, salt (transaction_a)));
    return result;
}

bool paper::confirm_req::operator == (paper::confirm_req const & other_a) const
{
    return *block == *other_a.block;
}

bool paper::publish::operator == (paper::publish const & other_a) const
{
    return *block == *other_a.block;
}

paper::fan::fan (paper::uint256_union const & key, size_t count_a)
{
    std::unique_ptr <paper::uint256_union> first (new paper::uint256_union (key));
    for (auto i (0); i != count_a; ++i)
    {
        std::unique_ptr <paper::uint256_union> entry (new paper::uint256_union);
        random_pool.GenerateBlock (entry->bytes.data (), entry->bytes.size ());
        *first ^= *entry;
        values.push_back (std::move (entry));
    }
    values.push_back (std::move (first));
}

paper::uint256_union paper::fan::value ()
{
    paper::uint256_union result;
    result.clear ();
    for (auto & i: values)
    {
        result ^= *i;
    }
    return result;
}

void paper::fan::value_set (paper::uint256_union const & value_a)
{
    auto value_l (value ());
    *(values [0]) ^= value_l;
    *(values [0]) ^= value_a;
}

std::array <uint8_t, 2> constexpr paper::message::magic_number;
size_t constexpr paper::message::ipv4_only_position;
size_t constexpr paper::message::bootstrap_server_position;
std::bitset <16> constexpr paper::message::block_type_mask;

paper::message::message (paper::message_type type_a) :
version_max (0x01),
version_using (0x01),
version_min (0x01),
type (type_a)
{
}

paper::message::message (bool & error_a, paper::stream & stream_a)
{
	error_a = read_header (stream_a, version_max, version_using, version_min, type, extensions);
}

paper::block_type paper::message::block_type () const
{
    return static_cast <paper::block_type> (((extensions & block_type_mask) >> 8).to_ullong ());
}

void paper::message::block_type_set (paper::block_type type_a)
{
    extensions &= ~paper::message::block_type_mask;
    extensions |= std::bitset <16> (static_cast <unsigned long long> (type_a) << 8);
}

bool paper::message::ipv4_only ()
{
    return extensions.test (ipv4_only_position);
}

void paper::message::ipv4_only_set (bool value_a)
{
    extensions.set (ipv4_only_position, value_a);
}

void paper::message::write_header (paper::stream & stream_a)
{
    paper::write (stream_a, paper::message::magic_number);
    paper::write (stream_a, version_max);
    paper::write (stream_a, version_using);
    paper::write (stream_a, version_min);
    paper::write (stream_a, type);
    paper::write (stream_a, static_cast <uint16_t> (extensions.to_ullong ()));
}

bool paper::message::read_header (paper::stream & stream_a, uint8_t & version_max_a, uint8_t & version_using_a, uint8_t & version_min_a, paper::message_type & type_a, std::bitset <16> & extensions_a)
{
    std::array <uint8_t, 2> magic_number_l;
    auto result (paper::read (stream_a, magic_number_l));
    if (!result)
    {
        result = magic_number_l != magic_number;
        if (!result)
        {
            result = paper::read (stream_a, version_max_a);
            if (!result)
            {
                result = paper::read (stream_a, version_using_a);
                if (!result)
                {
                    result = paper::read (stream_a, version_min_a);
                    if (!result)
                    {
                        result = paper::read (stream_a, type_a);
						if (!result)
						{
							uint16_t extensions_l;
							result = paper::read (stream_a, extensions_l);
							if (!result)
							{
								extensions_a = extensions_l;
							}
						}
                    }
                }
            }
        }
    }
    return result;
}

paper::landing_store::landing_store ()
{
}

paper::landing_store::landing_store (paper::account const & source_a, paper::account const & destination_a, uint64_t start_a, uint64_t last_a) :
source (source_a),
destination (destination_a),
start (start_a),
last (last_a)
{
}

paper::landing_store::landing_store (bool & error_a, std::istream & stream_a)
{
	error_a = deserialize (stream_a);
}

bool paper::landing_store::deserialize (std::istream & stream_a)
{
	bool result;
	try
	{
		boost::property_tree::ptree tree;
		boost::property_tree::read_json (stream_a, tree);
		auto source_l (tree.get <std::string> ("source"));
		auto destination_l (tree.get <std::string> ("destination"));
		auto start_l (tree.get <std::string> ("start"));
		auto last_l (tree.get <std::string> ("last"));
		result = source.decode_base58check (source_l);
		if (!result)
		{
			result = destination.decode_base58check (destination_l);
			if (!result)
			{
				start = std::stoull (start_l);
				last = std::stoull (last_l);
			}
		}
	}
	catch (std::logic_error const &)
	{
		result = true;
	}
	catch (std::runtime_error const &)
	{
		result = true;
	}
	return result;
}

void paper::landing_store::serialize (std::ostream & stream_a) const
{
	boost::property_tree::ptree tree;
	tree.put ("source", source.to_base58check ());
	tree.put ("destination", destination.to_base58check ());
	tree.put ("start", std::to_string (start));
	tree.put ("last", std::to_string (last));
	boost::property_tree::write_json (stream_a, tree);
}

bool paper::landing_store::operator == (paper::landing_store const & other_a) const
{
	return source == other_a.source && destination == other_a.destination && start == other_a.start && last == other_a.last;
}

paper::landing::landing (paper::node & node_a, std::shared_ptr <paper::wallet> wallet_a, paper::landing_store & store_a, boost::filesystem::path const & path_a) :
path (path_a),
store (store_a),
wallet (wallet_a),
node (node_a)
{
}

void paper::landing::write_store ()
{
	std::ofstream store_file;
	store_file.open (path.string ());
	if (!store_file.fail ())
	{
		store.serialize (store_file);
	}
}

paper::uint128_t paper::landing::distribution_amount (uint64_t interval)
{
	// Halfing period ~= Exponent of 2 in secounds approixmately 1 year = 2^25 = 33554432
	// Interval = Exponent of 2 in seconds approximately 1 minute = 2^6 = 64
	uint64_t intervals_per_period (1 << (25 - interval_exponent));
	paper::uint128_t result;
	if (interval < intervals_per_period * 1)
	{
		// Total supply / 2^halfing period / intervals per period
		// 2^128 / 2^1 / (2^25 / 2^6)
		result = paper::uint128_t (1) << (127 - (25 - interval_exponent)); // 50%
	}
	else if (interval < intervals_per_period * 2)
	{
		result = paper::uint128_t (1) << (126 - (25 - interval_exponent)); // 25%
	}
	else if (interval < intervals_per_period * 3)
	{
		result = paper::uint128_t (1) << (125 - (25 - interval_exponent)); // 13%
	}
	else if (interval < intervals_per_period * 4)
	{
		result = paper::uint128_t (1) << (124 - (25 - interval_exponent)); // 6.3%
	}
	else if (interval < intervals_per_period * 5)
	{
		result = paper::uint128_t (1) << (123 - (25 - interval_exponent)); // 3.1%
	}
	else if (interval < intervals_per_period * 6)
	{
		result = paper::uint128_t (1) << (122 - (25 - interval_exponent)); // 1.6%
	}
	else if (interval < intervals_per_period * 7)
	{
		result = paper::uint128_t (1) << (121 - (25 - interval_exponent)); // 0.8%
	}
	else if (interval < intervals_per_period * 8)
	{
		result = paper::uint128_t (1) << (121 - (25 - interval_exponent)); // 0.8*
	}
	else
	{
		result = 0;
	}
	return result;
}

uint64_t paper::landing::seconds_since_epoch ()
{
	return std::chrono::duration_cast <std::chrono::seconds> (std::chrono::system_clock::now ().time_since_epoch ()).count ();
}

void paper::landing::distribute_one ()
{
	auto now (seconds_since_epoch ());
	auto error (false);
	while (!error && store.last + distribution_interval.count () < now)
	{
		auto amount (distribution_amount ((store.last - store.start) >> 6));
		error = wallet->send_sync (store.source, store.destination, amount);
		if (!error)
		{
			BOOST_LOG (node.log) << boost::str (boost::format ("Successfully distributed %1%") % amount);
			store.last += distribution_interval.count ();
			write_store ();
		}
		else
		{
			BOOST_LOG (node.log) << "Error while sending distribution\n";
		}
	}
}

void paper::landing::distribute_ongoing ()
{
	distribute_one ();
	BOOST_LOG (node.log) << "Waiting for next distribution cycle";
	node.service.add (std::chrono::system_clock::now () + sleep_seconds, [this] () {distribute_ongoing ();});
}

paper::thread_runner::thread_runner (boost::asio::io_service & service_a, paper::processor_service & processor_a)
{
	auto count (std::max <unsigned> (4, std::thread::hardware_concurrency ()));
	for (auto i (0); i < count; ++i)
	{
		threads.push_back (std::thread ([&service_a] ()
		{
			try
			{
				service_a.run ();
			}
			catch (...)
			{
				assert (false && "Unhandled service exception");
			}
		}));
	}
	for (auto i (0); i < count; ++i)
	{
		threads.push_back (std::thread ([&processor_a] ()
		{
			try
			{
				processor_a.run ();
			}
			catch (...)
			{
				assert (false && "Unhandled processor exception");
			}
		}));
	}
}

void paper::thread_runner::join ()
{
	for (auto &i : threads)
	{
		i.join ();
	}
}

std::chrono::seconds constexpr paper::landing::distribution_interval;
std::chrono::seconds constexpr paper::landing::sleep_seconds;
