#include <iostream>
#include <map>
#include <string>
#include <vector>

#include <boost/algorithm/string.hpp>
#include <boost/filesystem.hpp>
#include <boost/program_options.hpp>
#include <boost/signals2.hpp>

#include "config/config.h"
#include "logging/logging.h"
#include "primary/aktualizr.h"
#include "utilities/utils.h"

#include "virtualsecondary.h"

namespace bpo = boost::program_options;

bpo::variables_map parse_options(int argc, char *argv[]) {
  bpo::options_description description("A simple wrapper for libaktualizr");
  description.add_options()
      ("config,c", bpo::value<std::vector<boost::filesystem::path> >()->composing(), "configuration file or directory")
      ("help,h", "print help message")
      ("secondary-configs-dir", bpo::value<boost::filesystem::path>(), "directory containing Secondary ECU configuration files")
      ("loglevel", bpo::value<int>(), "set log level 0-5 (trace, debug, info, warning, error, fatal)");

  bpo::variables_map vm;
  std::vector<std::string> unregistered_options;
  bpo::basic_parsed_options<char> parsed_options =
    bpo::command_line_parser(argc, argv).options(description).allow_unregistered().run();
  bpo::store(parsed_options, vm);
  bpo::notify(vm);
  unregistered_options = bpo::collect_unrecognized(parsed_options.options, bpo::include_positional);

  if (vm.count("help") != 0 || !unregistered_options.empty()) {
    std::cout << description << "\n";
    exit(EXIT_FAILURE);
  }
  return vm;
}

void process_event(const std::shared_ptr<event::BaseEvent> &event) {
  static std::map<std::string, unsigned int> progress;

  if (event->isTypeOf<event::DownloadProgressReport>()) {
    const auto download_progress = dynamic_cast<event::DownloadProgressReport *>(event.get());
    if (progress.find(download_progress->target.sha256Hash()) == progress.end()) {
      progress[download_progress->target.sha256Hash()] = 0;
    }
    const unsigned int prev_progress = progress[download_progress->target.sha256Hash()];
    const unsigned int new_progress = download_progress->progress;
    if (new_progress > prev_progress) {
      progress[download_progress->target.sha256Hash()] = new_progress;
      std::cout << "Download progress for file " << download_progress->target.filename() << ": " << new_progress
                << "%\n";
    }
  } else if (event->isTypeOf<event::DownloadTargetComplete>()) {
    const auto download_complete = dynamic_cast<event::DownloadTargetComplete *>(event.get());
    std::cout << "Download complete for file " << download_complete->update.filename() << ": "
              << (download_complete->success ? "success" : "failure") << "\n";  // NOLINT(cppcoreguidelines-pro-bounds-array-to-pointer-decay, hicpp-no-array-decay)
    progress.erase(download_complete->update.sha256Hash());
  } else if (event->isTypeOf<event::InstallStarted>()) {
    const auto install_started = dynamic_cast<event::InstallStarted *>(event.get());
    std::cout << "Installation started for device " << install_started->serial.ToString() << "\n";
  } else if (event->isTypeOf<event::InstallTargetComplete>()) {
    const auto install_complete = dynamic_cast<event::InstallTargetComplete *>(event.get());
    std::cout << "Installation complete for device " << install_complete->serial.ToString() << ": "
              << (install_complete->success ? "success" : "failure") << "\n";  // NOLINT(cppcoreguidelines-pro-bounds-array-to-pointer-decay, hicpp-no-array-decay)
  } else if (event->isTypeOf<event::UpdateCheckComplete>()) {
    const auto check_complete = dynamic_cast<event::UpdateCheckComplete *>(event.get());
    std::cout << check_complete->result.updates.size() << " updates available\n";
  } else {
    std::cout << "Received " << event->variant << " event\n";
  }
}

void initSecondaries(Aktualizr *aktualizr, const boost::filesystem::path& config_file) {
  if (!boost::filesystem::exists(config_file)) {
    throw std::invalid_argument("Specified config file doesn't exist: " + config_file.string());
  }

  std::ifstream json_file_stream(config_file.string());
  Json::Value config;
  std::string errs;

  if (!Json::parseFromStream(Json::CharReaderBuilder(), json_file_stream, &config, &errs)) {
    throw std::invalid_argument("Failed to parse Secondary config file " + config_file.string() + ": " + errs);
  }

  for (auto it = config.begin(); it != config.end(); ++it) {
    std::string secondary_type = it.key().asString();

    if (secondary_type == Primary::VirtualSecondaryConfig::Type) {
      for (const auto& c: *it) {
        Primary::VirtualSecondaryConfig sec_cfg(c);
        auto sec = std::make_shared<Primary::VirtualSecondary>(sec_cfg);
        aktualizr->AddSecondary(sec);
      }
    } else {
      LOG_ERROR << "Unsupported type of Secondary: " << secondary_type << std::endl;
    }
  }
}

// This function is to exec a command in terminal as using system() but returning the result as a string
std::string exec(const char* cmd) {
    std::array<char, 128> buffer;
    std::string result;
    std::unique_ptr<FILE, decltype(&pclose)> pipe(popen(cmd, "r"), pclose);
    if (!pipe) {
        throw std::runtime_error("popen() failed!");
    }
    while (fgets(buffer.data(), buffer.size(), pipe.get()) != nullptr) {
        result += buffer.data();
    }
    return result;
}


int main(int argc, char *argv[]) {
  logger_init();
  logger_set_threshold(boost::log::trivial::info);
  LOG_INFO << "demo-app starting";

  try {
    bpo::variables_map commandline_map = parse_options(argc, argv);
    Config config(commandline_map);

    Aktualizr aktualizr(config);

    auto f_cb = [](const std::shared_ptr<event::BaseEvent> event) { process_event(event); };
    boost::signals2::scoped_connection conn(aktualizr.SetSignalHandler(f_cb));

    if (!config.uptane.secondary_config_file.empty()) {
      try {
        initSecondaries(&aktualizr, config.uptane.secondary_config_file);
      } catch (const std::exception &e) {
        LOG_ERROR << "Failed to init Secondaries: " << e.what();
        LOG_ERROR << "Exiting...";
        return EXIT_FAILURE;
      }
    }

    aktualizr.Initialize();

    const char *cmd_list = "Available commands: SendDeviceData, CheckUpdates, Download, Install, CampaignCheck, CampaignAccept, SecArduinoInstall, FullUpdateCycle, Pause, Resume, Abort";
    std::cout << cmd_list << std::endl;

    std::vector<Uptane::Target> current_updates;
    std::string buffer;
    
    std::string hashfirmwarevirtual_old;
    std::string hashfirmwarevirtual_new;
    std::string hashfirmwarearduino_old;
    std::string hashfirmwarearduino_new;
    std::string hashfirmwaredisplay_old;
    std::string hashfirmwaredisplay_new;
    
    while (std::getline(std::cin, buffer)) {
      std::vector<std::string> words;
      boost::algorithm::split(words, buffer, boost::is_any_of("\t "), boost::token_compress_on);
      std::string &command = words.at(0);
      boost::algorithm::to_lower(command);
      if (command == "senddevicedata") {
        aktualizr.SendDeviceData().get();
      } else if (command == "checkupdates") {
        auto result = aktualizr.CheckUpdates().get();
        current_updates = result.updates;
      } else if (command == "download") {
        aktualizr.Download(current_updates).get();
      } else if (command == "install") {
        // Compute the hash of old firmware and see if changes after installation
        hashfirmwarearduino_old = exec("md5sum /var/sota/arduino-usb/firmware-arduino.bin");
        hashfirmwarevirtual_old = exec("md5sum /var/sota/virtsec1/firmware-virtual.zip");
        hashfirmwaredisplay_old = exec("md5sum /var/sota/displayecu/firmware-display.zip");
        
        aktualizr.Install(current_updates).get();
        
        
        hashfirmwarearduino_new = exec("md5sum /var/sota/arduino-usb/firmware-arduino.bin");
        hashfirmwarevirtual_new = exec("md5sum /var/sota/virtsec1/firmware-virtual.zip");
        hashfirmwaredisplay_new = exec("md5sum /var/sota/displayecu/firmware-display.zip");
        
        // If the update is for the virtual secondary, extract the packet, else no
        if (hashfirmwarevirtual_old == hashfirmwarevirtual_new) {
        	std::cout << "\nNo updates for virtual secondary\n" << std::endl;
        }
        else {
            std::cout << "\nExtracting the update packet...\n" << std::endl;
            system("cd /var/sota/virtsec1/ && unzip -o firmware-virtual");
        }
        
        // If the update is for the display ECU, extract the packet and install the update, else no
        if (hashfirmwaredisplay_old == hashfirmwaredisplay_new) {
        	std::cout << "\nNo updates for display ECU\n" << std::endl;
        }
        else {
            std::cout << "\nExtracting the update packet for display ECU...\n" << std::endl;
            system("cd /var/sota/displayecu/ && unzip -o firmware-display");
            system("python3 /var/sota/displayecu/dashboard_update_routine.py");
        }
                
        // If change occurs, automatically start installation of firmware in the secondary
        if (hashfirmwarearduino_old == hashfirmwarearduino_new) {
            std::cout << "No updates for Arduino secondary to be installed" << std::endl;
        }
        else {
            std::cout << "Starting installation for Arduino secondary...\n" << std::endl;
            system("avrdude -v -p atmega328p -c arduino -P /dev/ttyACM0 -b 115200 -D -U flash:w:/var/sota/arduino-usb//firmware-arduino.bin:i");
            std::cout << "\nInstallation completed for Arduino secondary" << std::endl;
        }
        
        current_updates.clear();
        // Force to check again for updates, since otherwise the update procedure is not complete on server side
        auto result = aktualizr.CheckUpdates().get();
        current_updates = result.updates;       
      } else if (command == "campaigncheck") {
        aktualizr.CampaignCheck().get();
      } else if (command == "campaignaccept") {
        if (words.size() == 2) {
          aktualizr.CampaignControl(words.at(1), campaign::Cmd::Accept).get();
        } else {
          std::cout << "Error. Specify the campaign ID" << std::endl;
        }
      } else if (command == "gethandle") {
        for (auto& target : current_updates) {
          std::cout << "Installing file " << target.filename();
          auto handle = aktualizr.OpenStoredTarget(target);
          //custom_install(handle);
        }
      } else if (command == "secarduinoinstall") {
        std::cout << "Starting flash for Arduino with AVRdude\n\n\n";
        system("avrdude -v -p atmega328p -c arduino -P /dev/ttyACM0 -b 115200 -D -U flash:w:/var/sota/arduino-usb//firmware-arduino.bin:i");
        std::cout << "\n\nExiting, see you next time! \n\n\n";
      } else if (command == "fullupdatecycle") {
        // Perform automatically a full update cycle
        
        // CheckUpdates
        auto result = aktualizr.CheckUpdates().get();
        current_updates = result.updates;
        
        // Download
        aktualizr.Download(current_updates).get();
        
        //Install
        hashfirmwarearduino_old = exec("md5sum /var/sota/arduino-usb/firmware-arduino.bin");
        hashfirmwarevirtual_old = exec("md5sum /var/sota/virtsec1/firmware-virtual.zip");
        hashfirmwaredisplay_old = exec("md5sum /var/sota/displayecu/firmware-display.zip");
        aktualizr.Install(current_updates).get();
        
        hashfirmwarearduino_new = exec("md5sum /var/sota/arduino-usb/firmware-arduino.bin");
        hashfirmwarevirtual_new = exec("md5sum /var/sota/virtsec1/firmware-virtual.zip");
        hashfirmwaredisplay_new = exec("md5sum /var/sota/displayecu/firmware-display.zip");

        if (hashfirmwarevirtual_old == hashfirmwarevirtual_new) {
        	std::cout << "\nNo updates for virtual secondary\n" << std::endl;
        }
        else {
            std::cout << "\nExtracting the update packet...\n" << std::endl;
            system("cd /var/sota/virtsec1/ && unzip -o firmware-virtual");
        }
        if (hashfirmwaredisplay_old == hashfirmwaredisplay_new) {
        	std::cout << "\nNo updates for display ECU\n" << std::endl;
        }
        else {
            std::cout << "\nExtracting the update packet for display ECU...\n" << std::endl;
            system("cd /var/sota/displayecu/ && unzip -o firmware-display");
            system("python3 /var/sota/displayecu/dashboard_update_routine.py");
        }
        if (hashfirmwarearduino_old == hashfirmwarearduino_new) {
            std::cout << "No updates for Arduino secondary to be installed" << std::endl;
        }
        else {
            std::cout << "Starting installation for Arduino secondary...\n" << std::endl;
            system("avrdude -v -p atmega328p -c arduino -P /dev/ttyACM0 -b 115200 -D -U flash:w:/var/sota/arduino-usb//firmware-arduino.bin:i");
            std::cout << "\nInstallation completed for Arduino secondary" << std::endl;
        }
        
        current_updates.clear();
        // Force to check again for updates, since otherwise the update procedure is not complete on server side
        result = aktualizr.CheckUpdates().get();
        current_updates = result.updates;
      } else if (command == "pause") {
        aktualizr.Pause();
      } else if (command == "resume") {
        aktualizr.Resume();
      } else if (command == "abort") {
        aktualizr.Abort();
      } else if (!command.empty()) {
        std::cout << "Unknown command.\n";
        std::cout << cmd_list << std::endl;
      }
    }
    return EXIT_SUCCESS;
  } catch (const std::exception &ex) {
    LOG_ERROR << "Fatal error in demo-app: " << ex.what();
    return EXIT_FAILURE;
  }
}
