#include <paper/cli/daemon.hpp>

#include <paper/working.hpp>
#include <boost/property_tree/json_parser.hpp>
#include <iostream>
#include <fstream>

paper_daemon::daemon_config::daemon_config () :
rpc_enable (false)
{
}

void paper_daemon::daemon_config::serialize (std::ostream & output_a)
{
    boost::property_tree::ptree tree;
	tree.put ("rpc_enable", rpc_enable);
	boost::property_tree::ptree rpc_l;
	rpc.serialize_json (rpc_l);
	tree.add_child ("rpc", rpc_l);
	boost::property_tree::ptree node_l;
	node.serialize_json (node_l);
	tree.add_child ("node", node_l);
    boost::property_tree::write_json (output_a, tree);
}

paper_daemon::daemon_config::daemon_config (bool & error_a, std::istream & input_a)
{
    error_a = false;
    boost::property_tree::ptree tree;
	try
	{
        boost::property_tree::read_json (input_a, tree);
		rpc_enable = tree.get <bool> ("rpc_enable");
		auto node_l (tree.get_child ("node"));
		error_a = error_a || node.deserialize_json (node_l);
		auto rpc_l (tree.get_child ("rpc"));
		error_a = error_a || rpc.deserialize_json (rpc_l);
	}
	catch (std::runtime_error const &)
	{
		error_a = true;
	}
}

void paper_daemon::daemon::run ()
{
    auto working (paper::working_path ());
	boost::filesystem::create_directories (working);
    auto config_error (false);
    paper_daemon::daemon_config config;
    auto config_path ((working / "config.json").string ());
    std::ifstream config_file;
    config_file.open (config_path);
    if (!config_file.fail ())
    {
        config = paper_daemon::daemon_config (config_error, config_file);
    }
    else
    {
        std::ofstream config_file;
        config_file.open (config_path);
        if (!config_file.fail ())
        {
            config.serialize (config_file);
        }
    }
	std::unique_ptr <paper::thread_runner> runner;
    if (!config_error)
    {
        auto service (boost::make_shared <boost::asio::io_service> ());
        auto pool (boost::make_shared <boost::network::utils::thread_pool> ());
		paper::work_pool work;
        paper::processor_service processor;
        paper::node_init init;
        auto node (std::make_shared <paper::node> (init, service, working, processor, config.node, work));
        if (!init.error ())
        {
            node->start ();
            paper::rpc rpc (service, pool, *node, config.rpc);
            if (config.rpc_enable)
            {
                rpc.start ();
            }
			runner.reset (new paper::thread_runner (*service, processor));
			runner->join ();
        }
        else
        {
            std::cerr << "Error initializing node\n";
        }
    }
    else
    {
        std::cerr << "Error loading configuration\n";
    }
}
