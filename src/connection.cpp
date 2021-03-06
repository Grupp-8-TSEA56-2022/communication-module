#include <iostream>
#include <string>
#include <exception>

#include "connection.h"
#include "log.h"

using namespace boost::asio;
using namespace std;
using ip::tcp;
using std::cout;
using std::endl;

/* Check if a given key exists in an json object */
bool exists(const json& j, const std::string& key) {
    // This will throw an uncatchable error if json is malformed
    return j.find(key) != j.end();
}

Connection::Connection(int port)
: port{port}, io_service{}, acceptor{io_service, tcp::endpoint(tcp::v4(), port)},
  socket{io_service}, parameters{false}, manual_instruction{false},
  semi_instruction{false}, auto_instruction{false}, map_data{false}, emergency_stop{false},
  lost_connection{false}, parameter_configuration{}, manual_drive_instruction{},
  semi_drive_instruction{}, drive_mission{}, map{}, thread{}, mtx{} {
    acceptor.accept(socket);
    Logger::log(INFO, __FILE__, "Connection", "Connection established");
    thread = new std::thread(&Connection::read, this);
}

Connection::~Connection() {
    Logger::log(INFO, __FILE__, "Connection", "Connection terminated");
    reading.store(false);
    thread->join();
    delete thread;
}

void Connection::restart() {
    boost::asio::io_service io_service;
    tcp::acceptor acceptor(io_service, tcp::endpoint(tcp::v4(), port)); // listen for new connection
    tcp::socket socket(io_service);  // socket creation
    acceptor.accept(socket);  // waiting for connection
}

/* Recieve a string from the client, set new_instruction, create instruction object */
void Connection::read() {
    while (reading.load()) {
        try {
            // Continuously read until newline, create json object from string
            boost::asio::streambuf buf;
            Logger::log(DEBUG, __FILE__, "read", "Reading until new-line");
            boost::asio::read_until( socket, buf, "\n" );
            Logger::log(DEBUG, __FILE__, "read", "New-line recieved");
            std::string request = boost::asio::buffer_cast<const char*>(buf.data());
            Logger::log(DEBUG, __FILE__, "read", request);

            if (request == "STOP\n") {
                // Emergency stop recieved, kill car
                Logger::log(INFO, __FILE__, "read", "STOP recieved");
                emergency_stop.store(true);
                return;
            }

            json j{};
            try {
                j = json::parse(request);
            } catch (std::invalid_argument&) {
                Logger::log(ERROR, __FILE__, "read",
                            "Could not turn request into json object");
                continue;
            } catch (std::exception& e) {
                Logger::log(ERROR, __FILE__, "read",
                            "Uncaught exception in json parsing");
                Logger::log(DEBUG, __FILE__, "read", e.what());
                break;
            }

            // Check what kind of response and create instance of appropriate class
            if (exists(j, "ManualDriveInstruction")) {
                std::lock_guard<std::mutex> lk(mtx);
                manual_instruction.store(true);
                ManualDriveInstruction inst{j};
                manual_drive_instruction = inst;
            } else if (exists(j, "SemiDriveInstruction")) {
                std::lock_guard<std::mutex> lk(mtx);
                semi_instruction.store(true);
                SemiDriveInstruction inst{j};
                semi_drive_instruction = inst;
            } else if (exists(j, "DriveMission")) {
                std::lock_guard<std::mutex> lk(mtx);
                auto_instruction.store(true);
                DriveMission inst{j};
                drive_mission = inst;                
            } else if (exists(j, "ParameterConfiguration")) {
                std::lock_guard<std::mutex> lk(mtx);
                parameters.store(true);
                ParameterConfiguration config{j};
                parameter_configuration = config;
            } else if (exists(j, "MapData")) {
                std::lock_guard<std::mutex> lk(mtx);
                map_data.store(true);
                map = j;
            }
        } catch (const boost::exception&) {
            Logger::log(ERROR, __FILE__, "read", "Connection lost");
            break;
        }
    }

    // Socket left loop, error has occured
    Logger::log(ERROR, __FILE__, "read", "Socket read interrupted");
    lost_connection.store(true);
}

void Connection::write(const std::string& response) {
    const std::string msg = response + "\n";
    boost::asio::write( socket, boost::asio::buffer(msg));
}

void Connection::write_formated(string const &label, string const &message) {
    ostringstream oss{};
    oss << "{\"" << label << "\":\"" << message << "\"}\n";
    boost::asio::write(socket, boost::asio::buffer(oss.str()));
}

void Connection::send_instruction_id(const std::string& id) {
    std::ostringstream oss;
    oss << "{\"InstructionId\": \"" << id << "\"}" ;
    write(oss.str());
}

bool Connection::has_lost_connection() {
    return lost_connection.load();
}

bool Connection::emergency_recieved() {
    return emergency_stop.load();
}

bool Connection::new_parameters() {
    return parameters.load();
}

bool Connection::new_manual_instruction() {
    return manual_instruction.load();
}

bool Connection::new_semi_instruction() {
    return semi_instruction.load();
}

bool Connection::new_auto_instruction() {
    return auto_instruction.load();
}

bool Connection::new_map() {
    return map_data.load();
}

/* Getters, sets new-values to false*/
ParameterConfiguration Connection::get_parameter_configuration() {
    std::lock_guard<std::mutex> lk(mtx);
    parameters.store(false);
    Logger::log(DEBUG, __FILE__, "Steering_kp", parameter_configuration.steering_kp);
    Logger::log(DEBUG, __FILE__, "Steering_kd", parameter_configuration.steering_kd);
    Logger::log(DEBUG, __FILE__, "Speed_kp", parameter_configuration.speed_kp);
    Logger::log(DEBUG, __FILE__, "Speed_ki", parameter_configuration.speed_ki);
    Logger::log(DEBUG, __FILE__, "Turn_kd", parameter_configuration.turn_kd);
    Logger::log(DEBUG, __FILE__, "Angle_offset", parameter_configuration.angle_offset);
    return parameter_configuration;
}

ManualDriveInstruction Connection::get_manual_drive_instruction() {
    std::lock_guard<std::mutex> lk(mtx);
    manual_instruction.store(false);
    Logger::log(INFO, __FILE__, "Throttle", manual_drive_instruction.throttle);
    Logger::log(INFO, __FILE__, "Steering", manual_drive_instruction.steering);
    return manual_drive_instruction;
}

SemiDriveInstruction Connection::get_semi_drive_instruction() {
    std::lock_guard<std::mutex> lk(mtx);
    semi_instruction.store(false);
    Logger::log(INFO, __FILE__, "Direction", semi_drive_instruction.direction);
    Logger::log(INFO, __FILE__, "Id", semi_drive_instruction.id);
    return semi_drive_instruction;
}

DriveMission Connection::get_drive_mission() {
    std::lock_guard<std::mutex> lk(mtx);
    auto_instruction.store(false);
    return drive_mission;
}

json Connection::get_map() {
    std::lock_guard<std::mutex> lk(mtx);
    map_data.store(false);
    return map;
}
