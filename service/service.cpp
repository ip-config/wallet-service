// Copyright 2018 The Beam Team
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//    http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#ifndef NDEBUG
#define LOG_VERBOSE_ENABLED 1
#endif

#include "service.h"
#include "service_client.h"
#include "utils.h"
#include "version.h"
#include "utility/cli/options.h"
#include "utility/logger.h"
#include "utility/log_rotation.h"
#include <boost/filesystem.hpp>
#include <boost/program_options.hpp>
#include <thread>
#include "reactor.h"

#ifdef BEAM_ATOMIC_SWAP_SUPPORT
// IWallet data has the following method
// virtual const IAtomicSwapProvider& getAtomicSwapProvider() const = 0;
// Its implementation would throw. If you're going to implement it notice
// that any API method can be invoked before open_wallet/create_wallet while
// wallet still does not exists/not opened. Consider refactoring to ::Ptr if necessary
#error "Atomic swaps are not supported in the wallet service"
#endif  // BEAM_ATOMIC_SWAP_SUPPORT

namespace
{
    using namespace beam;
    using namespace beam::wallet;

    class WalletService
        : public WebSocketServer
    {
    public:
        WalletService(SafeReactor::Ptr reactor, bool withAssets, const io::Address &nodeAddr, uint16_t port,
                      const std::string &allowedOrigin, bool withPipes)
                : WebSocketServer(std::move(reactor), port, "Wallet service", withPipes, allowedOrigin)
                , _withAssets(withAssets)
                , _nodeAddr(nodeAddr)
        {
        }

        ~WalletService() = default;

    private:
        WebSocketServer::ClientHandler::Ptr ReactorThread_onNewWSClient(WebSocketServer::SendFunc wsSend) override
        {
            return std::make_shared<ServiceClient>(_withAssets, _nodeAddr,  wsSend, _walletMap);
        }

    private:
        bool         _withAssets;
        io::Address  _nodeAddr;
        WalletMap    _walletMap;
    };
}

int main(int argc, char* argv[])
{
    using namespace beam;
    namespace po = boost::program_options;

    const char* LOG_FILES_DIR = "logs";
    const char* LOG_FILES_PREFIX = "service_";

    const auto path = boost::filesystem::system_complete(LOG_FILES_DIR);
    const auto logPrefix = std::string(LOG_FILES_PREFIX) + std::to_string(getCurrentPID()) + std::string("_");
    auto logger = beam::Logger::create(LOG_LEVEL_DEBUG, LOG_LEVEL_DEBUG, LOG_LEVEL_DEBUG, logPrefix, path.string());
    activateCrashLog();

    try
    {
        struct
        {
            uint16_t port;
            std::string nodeURI;
            Nonnegative<uint32_t> pollPeriod_ms;
            uint32_t logCleanupPeriod;
            std::string allowedOrigin;
            bool withPipes = false;
            bool withAssets = false;
        } options;

        io::Address node_addr;

        {
            po::options_description desc("Wallet API general options");
            desc.add_options()
                (cli::HELP_FULL, "list of all options")
                (cli::PORT_FULL, po::value(&options.port)->default_value(8080), "port to start server on")
                (cli::NODE_ADDR_FULL, po::value<std::string>(&options.nodeURI), "address of node")
                (cli::ALLOWED_ORIGIN, po::value<std::string>(&options.allowedOrigin)->default_value(""), "allowed origin")
                (cli::LOG_CLEANUP_DAYS, po::value<uint32_t>(&options.logCleanupPeriod)->default_value(5), "old logfiles cleanup period(days)")
                (cli::NODE_POLL_PERIOD, po::value<Nonnegative<uint32_t>>(&options.pollPeriod_ms)->default_value(Nonnegative<uint32_t>(0)), "Node poll period in milliseconds. Set to 0 to keep connection. Anyway poll period would be no less than the expected rate of blocks if it is less then it will be rounded up to block rate value.")
                (cli::WITH_SYNC_PIPES,  po::bool_switch(&options.withPipes)->default_value(false), "enable sync pipes")
                (cli::WITH_ASSETS, po::bool_switch(&options.withAssets)->default_value(false), "enable confidential assets transactions")
            ;

            desc.add(createRulesOptionsDescription());

            po::variables_map vm;

            po::store(po::command_line_parser(argc, argv)
                .options(desc)
                .style(po::command_line_style::default_style ^ po::command_line_style::allow_guessing)
                .run(), vm);

            if (vm.count(cli::HELP))
            {
                std::cout << desc << std::endl;
                return 0;
            }

            ReadCfgFromFileCommon(vm, desc);
            ReadCfgFromFile(vm, desc, "wallet-api.cfg");
            vm.notify();

            clean_old_logfiles(LOG_FILES_DIR, LOG_FILES_PREFIX, beam::wallet::days2sec(options.logCleanupPeriod));

            getRulesOptions(vm);
            Rules::get().UpdateChecksum();
            LOG_INFO() << "Beam Wallet API Service " << PROJECT_VERSION << " (" << BRANCH_NAME << ")";
            LOG_INFO() << "Rules signature: " << Rules::get().get_SignatureStr();
            LOG_INFO() << "Current folder is " << boost::filesystem::current_path().string();

            #ifdef NDEBUG
            LOG_INFO() << "Log mode: Non-Debug";
            #else
            LOG_INFO() << "Log mode: Debug";
            #endif
            
            if (vm.count(cli::NODE_ADDR) == 0)
            {
                LOG_ERROR() << "node address should be specified";
                return -1;
            }

            if (!node_addr.resolve(options.nodeURI.c_str()))
            {
                LOG_ERROR() << "unable to resolve node address: `" << options.nodeURI << "`";
                return -1;
            }
        }

        SafeReactor::Ptr safeReactor = SafeReactor::create();
        io::Reactor::Scope scope(safeReactor->ref());
        io::Reactor::GracefulIntHandler gih(safeReactor->ref());

        const unsigned LOG_ROTATION_PERIOD_SEC = 12 * 60 * 60; // in seconds, 12 hours
        LogRotation logRotation(safeReactor->ref(), LOG_ROTATION_PERIOD_SEC, beam::wallet::days2sec(options.logCleanupPeriod));
        LOG_INFO() << "Log rotation: " <<  beam::wallet::sec2readable(LOG_ROTATION_PERIOD_SEC) << ". Log cleanup: " << options.logCleanupPeriod << " days.";
        LOG_INFO() << "Starting server on port " << options.port << ", sync pipes " << options.withPipes;

        WalletService server(safeReactor, options.withAssets, node_addr, options.port, options.allowedOrigin, options.withPipes);
        safeReactor->ref().run();

        LOG_INFO() << "Done";
    }
    catch (const std::exception& e)
    {
        LOG_ERROR() << "EXCEPTION: " << e.what();
    }
    catch (...)
    {
        LOG_ERROR() << "NON_STD EXCEPTION";
    }

    return 0;
}
