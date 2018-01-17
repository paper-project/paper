#include <paper/qt/qt.hpp>

#include <paper/working.hpp>
#include <paper/icon.hpp>
#include <boost/property_tree/ptree.hpp>
#include <boost/property_tree/json_parser.hpp>

class qt_wallet_config
{
public:
	qt_wallet_config () :
	account (0)
	{
		paper::random_pool.GenerateBlock (wallet.bytes.data (), wallet.bytes.size ());
		assert (!wallet.is_zero ());
	}
	qt_wallet_config (bool & error_a, std::istream & stream_a)
	{
		error_a = false;
		boost::property_tree::ptree tree;
		try
		{
			boost::property_tree::read_json (stream_a, tree);
			auto wallet_l (tree.get <std::string> ("wallet"));
			auto account_l (tree.get <std::string> ("account"));
			auto node_l (tree.get_child ("node"));
			try
			{
				error_a = error_a | wallet.decode_hex (wallet_l);
				error_a = error_a | account.decode_base58check (account_l);
				error_a = error_a | node.deserialize_json (node_l);
			}
			catch (std::logic_error const &)
			{
				error_a = true;
			}
		}
		catch (std::runtime_error const &)
		{
		    std::cout << "Error parsing config file" << std::endl;
		    error_a = true;
		}
	}
	void serialize (std::ostream & stream_a)
	{
		boost::property_tree::ptree tree;
		std::string wallet_string;
		wallet.encode_hex (wallet_string);
		tree.put ("wallet", wallet_string);
		tree.put ("account", account.to_base58check ());
		boost::property_tree::ptree node_l;
		node.serialize_json (node_l);
		tree.add_child ("node", node_l);
		boost::property_tree::write_json (stream_a, tree);
	}
	paper::uint256_union wallet;
	paper::account account;
	paper::node_config node;
};

int main (int argc, char * const * argv)
{
	auto working (paper::working_path ());
	boost::filesystem::create_directories (working);
	auto config_error (false);
	qt_wallet_config config;
	auto config_path ((working / "config.json").string ());
	std::ifstream config_file;
	config_file.open (config_path);
	auto uninitialized (true);
	if (!config_file.fail ())
	{
		config = qt_wallet_config (config_error, config_file);
		uninitialized = false;
	}
    if (!config_error)
    {
        QApplication application (argc, const_cast <char **> (argv));
		paper::set_application_icon (application);
        auto service (boost::make_shared <boost::asio::io_service> ());
		paper::work_pool work;
        paper::processor_service processor;
        paper::node_init init;
        auto node (std::make_shared <paper::node> (init, service, working, processor, config.node, work));
        if (!init.error ())
        {
            if (uninitialized || config.wallet.is_zero ())
            {
				if (config.wallet.is_zero ())
				{
					paper::random_pool.GenerateBlock (config.wallet.bytes.data (), config.wallet.bytes.size ());
				}
                auto wallet (node->wallets.create (config.wallet));
                paper::keypair key;
                config.account = key.pub;
                wallet->insert (key.prv);
				assert (wallet->exists (config.account));
                std::ofstream config_file;
                config_file.open (config_path);
                if (!config_file.fail ())
                {
                    config.serialize (config_file);
                }
            }
            auto wallet (node->wallets.open (config.wallet));
            if (wallet != nullptr)
            {
                if (wallet->exists (config.account))
                {
                    QObject::connect (&application, &QApplication::aboutToQuit, [&] ()
                    {
                        node->stop ();
                    });
                    node->start ();
                    std::unique_ptr <paper_qt::wallet> gui (new paper_qt::wallet (application, *node, wallet, config.account));
                    gui->client_window->show ();
					paper::thread_runner runner (*service, processor);
                    int result;
                    try
                    {
                        result = application.exec ();
                    }
                    catch (...)
                    {
                        result = -1;
                        assert (false);
                    }
					runner.join ();
                    return result;
                }
                else
                {
                    std::cerr << "Wallet account doesn't exist";
                }
            }
            else
            {
                std::cerr << "Wallet id doesn't exist";
            }
        }
        else
        {
            std::cerr << "Error initializing node\n";
        }
    }
    else
    {
        std::cerr << "Error in config file";
    }
}
