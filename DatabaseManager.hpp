#pragma once

#include <pqxx/pqxx>
#include <string>
#include <memory>
#include <stdexcept>
#include <iostream>
// A simple connection manager.
// For a high-scale server, you would expand this into a full connection pool.
class DatabaseManager {
private:
    std::string connection_string_;

public:
    DatabaseManager(const std::string& conn_str)
        : connection_string_(conn_str) {
        try {
            // Test the connection on startup
            pqxx::connection C(connection_string_);
            std::cout << "Database connected successfully to: " << C.dbname() << std::endl;
        }
        catch (const std::exception& e) {
            std::cerr << "Database connection failed: " << e.what() << std::endl;
            throw; // Re-throw to stop the server if DB connection fails
        }
    }

    // A function for sessions to get a new connection
    pqxx::connection get_connection() {
        try {
            return pqxx::connection(connection_string_);
        }
        catch (const std::exception& e) {
            std::cerr << "Failed to create new connection: " << e.what() << std::endl;
            // Re-throw as a runtime error to be caught by the session
            throw std::runtime_error("Could not connect to database.");
        }
    }
};