// Andrew Miller, UIUC

#include "bootstrap_file.h"
#include "blocksdat_file.h"
#include "common/command_line.h"
#include "cryptonote_core/tx_pool.h"
#include "blockchain_db/blockchain_db.h"
#include "blockchain_db/lmdb/db_lmdb.h"
#if defined(BERKELEY_DB)
#include "blockchain_db/berkeleydb/db_bdb.h"
#endif
#include "blockchain_db/db_types.h"
#include "version.h"

namespace po = boost::program_options;
using namespace epee; // log_space

namespace
{
  std::string refresh_string = "\r                                    \r";
}

bool parse_blockchain_pass1(Blockchain* blockchain_storage, tx_memory_pool* _tx_pool, boost::filesystem::path& output_file, uint64_t requested_block_stop) {
  uint64_t cur_height = 0;
  uint64_t num_blocks_written = 0;
  uint64_t progress_interval = 100;
  block b;

  uint64_t block_start = 0;
  uint64_t block_stop = 0;
  LOG_PRINT_L0("source blockchain height: " <<  blockchain_storage->get_current_blockchain_height()-1);
  if ((requested_block_stop > 0) && (requested_block_stop < blockchain_storage->get_current_blockchain_height()))
  {
    LOG_PRINT_L0("Using requested block height: " << requested_block_stop);
    block_stop = requested_block_stop;
  }
  else
  {
    block_stop = blockchain_storage->get_current_blockchain_height() - 1;
    LOG_PRINT_L0("Using block height of source blockchain: " << block_stop);
  }
  LOG_PRINT_L0("Storing blocks raw data...");

  for (cur_height = block_start; cur_height <= block_stop; ++cur_height)
  {
    // this method's height refers to 0-based height (genesis block = height 0)
    crypto::hash hash = blockchain_storage->get_block_id_by_height(cur_height);

    cryptonote::block blk;
    
    try {
      blk = blockchain_storage->get_db().get_block_from_height(cur_height);
    } catch (std::exception &e) {
      cerr << e.what() << endl;
      return false;
    }
    
    // get all transactions in the block found
    // initialize the first list with transaction for solving
    // the block i.e. coinbase.
    list<cryptonote::transaction> txs {blk.miner_tx};
    list<crypto::hash> missed_txs;
    
    if (!blockchain_storage->get_transactions(blk.tx_hashes, txs, missed_txs))
      {
	cerr << "Cant find transactions in block: " << cur_height << endl;
	return false;
      }
    
    for (const cryptonote::transaction& tx : txs)
      {
	crypto::hash tx_hash = get_transaction_hash(tx);

	LOG_PRINT_L0("\ntx hash          : " << tx_hash << ", block height " << cur_height << " " << tx.vin.size());

	vector<string> mixin_timescales_str;

	// total number of inputs in the transaction tx
	size_t input_no = tx.vin.size();

	for (size_t in_i = 0; in_i < input_no; ++in_i)
	  {
	    cryptonote::txin_v tx_in = tx.vin[in_i];

	    if (tx_in.type() == typeid(cryptonote::txin_gen))
	      {
		//LOG_PRINT_L0(" - coinbase tx: no inputs here.\n");
		continue;
	      }

	    // get tx input key
	    const cryptonote::txin_to_key& tx_in_to_key
	      = boost::get<cryptonote::txin_to_key>(tx_in);


	    //LOG_PRINT_L0("Input's xmr:" << in_i << " " << tx_in_to_key.amount);

	    // get absolute offsets of mixins
	    std::vector<uint64_t> absolute_offsets
	      = cryptonote::relative_output_offsets_to_absolute(
								tx_in_to_key.key_offsets);

	    std::vector<cryptonote::output_data_t> outputs;
	    blockchain_storage->get_db().get_output_key(tx_in_to_key.amount,
							absolute_offsets,
							outputs);

	    vector<uint64_t> mixin_timestamps;

	    size_t count = 0;

	    for (const uint64_t& i: absolute_offsets)
	      {

		cryptonote::output_data_t output_data;
		size_t output_index;
		bool output_at {true};

		// get tx hash and output index for output
		if (count < outputs.size())
		  {
		    output_data = outputs.at(count);
		  }

		// get block of given height, as we want to get its timestamp
		cryptonote::block blk;

		try {
		  blk = blockchain_storage->get_db().get_block_from_height(cur_height);
		} catch (std::exception &e) {
		  cerr << e.what() << endl;
		  return false;
		}

		//LOG_PRINT_L0("mixin no: " << (count + 1));

		++count;
	      } // for (const uint64_t& i: absolute_offsets)

	  } // for (size_t in_i = 0; in_i < input_no; ++in_i)	

      }
	
    //write_block(hash);
    if (cur_height % NUM_BLOCKS_PER_CHUNK == 0) {
      num_blocks_written += NUM_BLOCKS_PER_CHUNK;
    }		   
    
    if (cur_height % progress_interval == 0) {
      std::cout << refresh_string;
      std::cout << "block " << cur_height << "/" << block_stop << std::flush;
    }
  }
  // print message for last block, which may not have been printed yet due to progress_interval
  std::cout << refresh_string;
  std::cout << "block " << cur_height-1 << "/" << block_stop << ENDL;
  
  return true;
}

std::string join_set_strings(const std::unordered_set<std::string>& db_types_all, const char* delim)
{
  std::string result;
  std::ostringstream s;
  std::copy(db_types_all.begin(), db_types_all.end(), std::ostream_iterator<std::string>(s, delim));
  result = s.str();
  if (result.length() > 0)
    result.erase(result.end()-strlen(delim), result.end());
  return result;
}

int main(int argc, char* argv[])
{
  std::string default_db_type = "lmdb";

  std::unordered_set<std::string> db_types_all = cryptonote::blockchain_db_types;
  db_types_all.insert("memory");

  std::string available_dbs = join_set_strings(db_types_all, ", ");
  available_dbs = "available: " + available_dbs;

  uint32_t log_level = 0;
  uint64_t block_stop = 0;
  bool blocks_dat = false;

  tools::sanitize_locale();

  boost::filesystem::path default_data_path {tools::get_default_data_dir()};
  boost::filesystem::path default_testnet_data_path {default_data_path / "testnet"};
  boost::filesystem::path output_file_path;

  po::options_description desc_cmd_only("Command line options");
  po::options_description desc_cmd_sett("Command line options and settings options");
  const command_line::arg_descriptor<std::string> arg_output_file = {"output-file", "Specify output file", "", true};
  const command_line::arg_descriptor<uint32_t> arg_log_level  = {"log-level",  "", log_level};
  const command_line::arg_descriptor<uint64_t> arg_block_stop = {"block-stop", "Stop at block number", block_stop};
  const command_line::arg_descriptor<bool>     arg_testnet_on = {
    "testnet"
      , "Run on testnet."
      , false
  };
  const command_line::arg_descriptor<std::string> arg_database = {
    "database", available_dbs.c_str(), default_db_type
  };
  const command_line::arg_descriptor<bool> arg_blocks_dat = {"transactions.db", "Output in sqlite format", blocks_dat};


  command_line::add_arg(desc_cmd_sett, command_line::arg_data_dir, default_data_path.string());
  command_line::add_arg(desc_cmd_sett, command_line::arg_testnet_data_dir, default_testnet_data_path.string());
  command_line::add_arg(desc_cmd_sett, arg_output_file);
  command_line::add_arg(desc_cmd_sett, arg_testnet_on);
  command_line::add_arg(desc_cmd_sett, arg_log_level);
  command_line::add_arg(desc_cmd_sett, arg_database);
  command_line::add_arg(desc_cmd_sett, arg_block_stop);
  command_line::add_arg(desc_cmd_sett, arg_blocks_dat);

  command_line::add_arg(desc_cmd_only, command_line::arg_help);

  po::options_description desc_options("Allowed options");
  desc_options.add(desc_cmd_only).add(desc_cmd_sett);

  po::variables_map vm;
  bool r = command_line::handle_error_helper(desc_options, [&]()
  {
    po::store(po::parse_command_line(argc, argv, desc_options), vm);
    po::notify(vm);
    return true;
  });
  if (! r)
    return 1;

  if (command_line::get_arg(vm, command_line::arg_help))
  {
    std::cout << "Monero '" << MONERO_RELEASE_NAME << "' (v" << MONERO_VERSION_FULL << ")" << ENDL << ENDL;
    std::cout << desc_options << std::endl;
    return 1;
  }

  log_level    = command_line::get_arg(vm, arg_log_level);
  block_stop = command_line::get_arg(vm, arg_block_stop);

  log_space::get_set_log_detalisation_level(true, log_level);
  log_space::log_singletone::add_logger(LOGGER_CONSOLE, NULL, NULL);
  LOG_PRINT_L0("Starting...");
  LOG_PRINT_L0("Setting log level = " << log_level);

  bool opt_testnet = command_line::get_arg(vm, arg_testnet_on);
  bool opt_blocks_dat = command_line::get_arg(vm, arg_blocks_dat);

  std::string m_config_folder;

  auto data_dir_arg = opt_testnet ? command_line::arg_testnet_data_dir : command_line::arg_data_dir;
  m_config_folder = command_line::get_arg(vm, data_dir_arg);

  std::string db_type = command_line::get_arg(vm, arg_database);
  if (db_types_all.count(db_type) == 0)
  {
    std::cerr << "Invalid database type: " << db_type << std::endl;
    return 1;
  }
#if !defined(BERKELEY_DB)
  if (db_type == "berkeley")
  {
    LOG_ERROR("BerkeleyDB support disabled.");
    return false;
  }
#endif

  if (command_line::has_arg(vm, arg_output_file))
    output_file_path = boost::filesystem::path(command_line::get_arg(vm, arg_output_file));
  else
    output_file_path = boost::filesystem::path(m_config_folder) / "export" / "spri.db";
  LOG_PRINT_L0("Export output file: " << output_file_path.string());

  // If we wanted to use the memory pool, we would set up a fake_core.

  // Use Blockchain instead of lower-level BlockchainDB for two reasons:
  // 1. Blockchain has the init() method for easy setup
  // 2. exporter needs to use get_current_blockchain_height(), get_block_id_by_height(), get_block_by_hash()
  //
  // cannot match blockchain_storage setup above with just one line,
  // e.g.
  //   Blockchain* blockchain_storage = new Blockchain(NULL);
  // because unlike blockchain_storage constructor, which takes a pointer to
  // tx_memory_pool, Blockchain's constructor takes tx_memory_pool object.
  LOG_PRINT_L0("Initializing source blockchain (BlockchainDB)");
  Blockchain* blockchain_storage = NULL;
  tx_memory_pool m_mempool(*blockchain_storage);
  blockchain_storage = new Blockchain(m_mempool);

  int db_flags = 0;

  BlockchainDB* db = nullptr;
  if (db_type == "lmdb")
  {
    db_flags |= MDB_RDONLY;
    db = new BlockchainLMDB();
  }
#if defined(BERKELEY_DB)
  else if (db_type == "berkeley")
    db = new BlockchainBDB();
#endif
  else
  {
    LOG_ERROR("Attempted to use non-existent database type: " << db_type);
    throw std::runtime_error("Attempting to use non-existent database type");
  }
  LOG_PRINT_L0("database: " << db_type);

  boost::filesystem::path folder(m_config_folder);
  folder /= db->get_db_name();
  const std::string filename = folder.string();

  LOG_PRINT_L0("Loading blockchain from folder " << filename << " ...");
  try
  {
    db->open(filename, db_flags);
  }
  catch (const std::exception& e)
  {
    LOG_PRINT_L0("Error opening database: " << e.what());
    return 1;
  }
  r = blockchain_storage->init(db, opt_testnet);

  CHECK_AND_ASSERT_MES(r, false, "Failed to initialize source blockchain storage");
  LOG_PRINT_L0("Source blockchain storage initialized OK");
  LOG_PRINT_L0("Looping through blockchain....");

  r = parse_blockchain_pass1(blockchain_storage, NULL, output_file_path, block_stop);

  CHECK_AND_ASSERT_MES(r, false, "Failed to export blockchain raw data");
  LOG_PRINT_L0("Blockchain raw data exported OK");
}
