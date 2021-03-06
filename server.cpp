#include <iostream>
#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <unistd.h>
#include <regex>
#include <filesystem>
#include <ext/stdio_filebuf.h>
#include <sys/sendfile.h>
#include <signal.h>
#include <unordered_map>
#include "err.h"

#define C_OK 200
#define C_F_MOVED 302
#define C_REQ_ERROR 400
#define C_F_NOTFOUND 404
#define C_SERV_ERROR 500
#define C_BAD_METH 501

#define MAX_PORT_N 65535

#define BUFF_SIZE 4096
#define PORT_NUM 8080
#define TIMEOUT 45
#define debug(x)  if (debug) { \
                        std::cerr << x << std::endl; \
                   }

namespace fs = std::filesystem;

namespace {
    bool debug = 0;

    const std::string HTTP_version = "HTTP/1\\.1";
    const std::string CRLF = "\r\n";

    void r_trim(std::string& s) {
        s.erase(s.find_last_not_of(" ") + 1);
    }

    struct corelated_servers_exception : public std::exception {
        const char * what() const throw() {
            return "Corelated servers file not found!\n";
        }
    };

    struct base_dir_exception : public std::exception {
         const char * what() const throw() {
            return "Base directory not found!\n";
        }
    };

    struct sigpipe_exception : public std::exception {
        const char * what() const throw() {
            return "Pipe error!\n";
        }
    };

    struct invalid_path_exception : public std::exception {
        const char * what() const throw() {
            return "Invalid path provided!\n";
        }
    };

    struct file_not_found_exception : public std::exception {
        const char * what() const throw() {
            return "File not found\n";
        }
    };
}

class Socket {
    int server_socket;

public:
    Socket() {
        server_socket = socket(PF_INET, SOCK_STREAM, 0);

        if (server_socket < 0) {
            syserr("Creating server socket");
        }
    }

    ~Socket() {
        if (close(server_socket) < 0) {
            syserr("Closing server socket.");
        }
    }

    void bind_sock(sockaddr_in &server_address) {
        //Bind socket to the server.
        if (bind(server_socket, reinterpret_cast<const sockaddr *>(&server_address), sizeof(server_address)) < 0) {
            syserr("Binding server socket.");
        }
    }

    void start_listen(int q_len) {
        if (listen(server_socket, q_len) < 0) {
            syserr("Starting to listen.");
        }
    }

    int get_socket_num() {
        return server_socket;
    }
};


class Client {
    int client_socket;
    struct sockaddr_in client_address;
    socklen_t client_addres_len;
    __gnu_cxx::stdio_filebuf<char> client_buf;

public:
    Client(int socket) {
        client_addres_len = sizeof (client_address);

        client_socket = accept(socket, (struct sockaddr *) &client_address, &client_addres_len);

        client_buf = __gnu_cxx::stdio_filebuf<char>(client_socket, std::ios::in);

        struct timeval timeout;
        timeout.tv_sec = TIMEOUT;
        timeout.tv_usec = 0;

        if (setsockopt(client_socket, SOL_SOCKET, SO_RCVTIMEO, (char *)&timeout, sizeof(timeout)) < 0) {
            syserr("Creating timeout on client socket");
        }

        if (client_socket < 0) {
            syserr("Accepting client");
        }
    }

    ~Client() {
        if (close(client_socket) < 0){
            syserr("Closing client socket.");
        }
    }

    __gnu_cxx::stdio_filebuf<char>& get_client_buf() {
        return client_buf;
    }

    void send_message(std::string message) const {
        int len = message.length();

        while (len > 0) {
            int w_len = write(client_socket, message.c_str(), len);

            if (w_len < 0) {
                throw sigpipe_exception();
            }

            len -= w_len;

            message.erase(0, w_len);
        }

    }

    void send_file(const char *file_path) const {
        FILE *file_ = fopen(file_path, "r");
        int len;

        do {
            len = sendfile(client_socket, fileno(file_), NULL, BUFF_SIZE);

            if (len < 0) {
                throw sigpipe_exception();
            }
        } while (len > 0);
    }

};

class Directory {
public:
    std::filesystem::path root_path;

public:
    Directory(char *root_path) : root_path(root_path) {
        if (!std::filesystem::exists(root_path)) {
            throw base_dir_exception();
        }
    }

    std::filesystem::path normalized_trimed(const std::filesystem::path& p) {
        auto r = p.lexically_normal();

        if (r.has_filename()) {
            return r;
        }

        return r.parent_path();
    }

    bool is_subpath_of(const std::filesystem::path& base, const std::filesystem::path& sub) {
        auto b = normalized_trimed(base);
        auto s = normalized_trimed(sub).parent_path();
        auto m = std::mismatch(b.begin(), b.end(),
                               s.begin(), s.end());

        return m.first == b.end();
    }

    std::ifstream check_existence(const std::filesystem::path& file) {
        const static std::regex path_regex("(/([a-zA-Z0-9.\\-]+))+");

        std::filesystem::path full_path(root_path);

        full_path += file;

        if (!is_subpath_of(root_path, full_path) || !std::regex_match(file.string(), path_regex)) {
            throw invalid_path_exception();
        }

        // Check if file exists in local directory.
        if (std::filesystem::exists(full_path)) {
            std::ifstream f_stream(full_path.c_str());

            // Check if we can read from the file.
            if (f_stream.good()) {
                return f_stream;
            }
        }

        debug("File is not located locally")
        throw file_not_found_exception();
    }

    std::uintmax_t size(const std::filesystem::path& file) {
        std::filesystem::path full_path(root_path);

        full_path += file;

        return std::filesystem::file_size(full_path);
    }
};

class Foreign_resources {
    std::unordered_map<std::string, std::string> res_map;

public:
    Foreign_resources(char* path) {
        std::filesystem::path res_path(path);

        if (std::filesystem::exists(res_path)) {
            std::fstream res_file(res_path.c_str());

            if (res_file.good()) {
                std::string resource, server, port;

                while (res_file >> resource >> server >> port) {
                    debug("ADDING TO MAP: " << resource << " --> " << server << ":" <<port);

                    if (res_map.find(resource) == res_map.end()) {
                        res_map[resource] = server + ":" + port;
                    }
                }
            }
        }
        else {
            throw corelated_servers_exception();
        }
    }

    std::string find(const std::string& path) {
        auto it = res_map.find(path);
        debug("Searching for file online: " << path);

        if (it == res_map.end()) {
            return "";
        }
        else {
            return "http://" + it->second + path;
        }
    }


};

class Codes_detailed {
    std::unordered_map<int, std::string> phrase_map;

public:
    Codes_detailed() {
        phrase_map[C_OK] = "Requested file has been found!";
        phrase_map[C_F_MOVED] = "Requested file has been moved to another server...";
        phrase_map[C_REQ_ERROR] = "Invalid request format.";
        phrase_map[C_F_NOTFOUND] = "Requested file not found.";
        phrase_map[C_SERV_ERROR] = "SERVER ERROR!";
        phrase_map[C_BAD_METH] = "Unknown method provided.";
    }

    const std::string& get_description(int code) {
        return phrase_map[code];
    }
};

class HTTP_request {
public:
    Codes_detailed codesDetailed;
    std::string method;
    std::string file;
    int code;
    bool kill;
    bool appeared[2];

    //Empty request is invalid, so we have to set the flags as if error occured already.
    HTTP_request() : code(C_REQ_ERROR), kill(true), appeared{false, false} {};

    //Get a line to parse, and
    bool parse_line(const std::string& line) {
        // If method is empty, then we know that we've just started parsing,
        // so we need to parse this line as start-line.
        if (method.empty()) {
            kill = false;

            return parse_start_line(line);
        }
        // Otherwise, we parse it as the header.
        else {
            return parse_header_line(line);
        }
    }

    std::string create_response(std::uintmax_t f_size, const std::string &f_location) {
        std::ostringstream response;

        response << "HTTP/1.1 " << code << " " << codesDetailed.get_description(code) << CRLF;

        if (code == 200) {
            response << "Content-type: application/octet-stream" << CRLF;
            response << "Content-length: " << f_size << CRLF;
        }

        if (code == 302) {
            response << "Location: " << f_location << CRLF;
        }

        response << "Server: SiK-Zad1" << CRLF;

        if (kill) {
            response << "Connection: close" << CRLF;
        }

        response << CRLF;


        return response.str();
    }

    bool parse_start_line(const std::string& start_line) {
        if (!check_if_valid_line(start_line)) {
            debug("LINE NOT ENDED WITH CRLF!")

            set_error(C_REQ_ERROR);

            return false;
        }

        static const std::regex start_pattern(R"(([^ ]+) (/[^ ]*) )" + HTTP_version + "\r");

        std::smatch groups;

        if(std::regex_match(start_line, groups, start_pattern)) {
            method = groups[1].str();
            file = groups[2].str();

            if (method == "GET" || method == "HEAD") {
                debug("METHOD CORRECT!");

                code = C_OK;
            }
            else {
                debug("METHOD INCORRECT!");

                set_error(C_BAD_METH);

                return false;
            }

            return true;
        }
        else {
            debug("START-LINE REGEX NOT MATCHED");
            set_error(C_REQ_ERROR);

            return false;
        }
    }

private:
    //Check if the line ends with CRLF. (We know that getline function, trims the newline char,
    //so we have to check if the last is '\r').
    bool check_if_valid_line(const std::string& line) {
        int len = line.length();

        return line[len - 1] == '\r';
    }

    void set_error(int err_code) {
        code = err_code;

        kill = true;
    }

    bool parse_header_line(const std::string& header) {
        if (!check_if_valid_line(header)) {
            set_error(C_REQ_ERROR);

            return false;
        }

        static const std::regex header_pattern("([^:]+): *(.*)\r");
        static const std::regex CONN("CONNECTION", std::regex_constants::icase);
        static const std::regex CONTENT_L("CONTENT-LENGTH", std::regex_constants::icase);
        static const std::regex ZERO("0+");

        std::string h_name, h_value;
        std::smatch groups;

        //Try to match the header regex, and save submatches into "groups".
        if (std::regex_match(header, groups, header_pattern)) {
            h_name = groups[1].str();       //Save the first submatch which is the header name.

            debug("CONTENT NAME: " << h_name)

            //Check if there is more than one submatch, if so, then we know that there is a header-value associated
            //with the header name.
            if (groups.length() > 2) {
                h_value = groups[2].str();
                r_trim(h_value);            //Remove white spaces from the right end of the string.

                debug("CONTENT VALUE: " << h_value)
            }

            if (std::regex_match(h_name, CONN)) {
                if (appeared[0]) {
                    set_error(C_REQ_ERROR);

                    debug("HEADER DUPLICATED!")

                    return false;
                }

                appeared[0] = true;

                if (h_value == "close") {
                    kill = true;
                }

                return true;
            }
            else if (std::regex_match(h_name, CONTENT_L)) {
                if (appeared[1]) {
                    set_error(C_REQ_ERROR);

                    debug("HEADER DUPLICATED!")

                    return false;
                }

                appeared[1] = true;

                //If content-length value is different than 0, then we should mark this request as failed one.
                if (std::regex_match(h_value, ZERO)) {
                    return true;
                }
                else  {
                    set_error(C_REQ_ERROR);

                    return false;
                }
            }
            else {
                return true;
            }
        }
        else {
            debug("HEADER REGEX DIDNT MATCH")

            set_error(C_REQ_ERROR);

            return false;
        }
    }

};

class TCP_Server {
    struct sockaddr_in server_address;
    Socket server_socket;
    Directory base_dir;
    Foreign_resources foreign_resources;

public:
    TCP_Server(char *base_dir, char *server_files, int port_num) : base_dir(base_dir),
                                                                   foreign_resources(server_files) {
        server_address.sin_family = AF_INET;
        server_address.sin_port = htons(port_num);
        server_address.sin_addr.s_addr = htonl(INADDR_ANY);

        server_socket.bind_sock(server_address);
        server_socket.start_listen(5);

        std::clog << "Sever created with port: " << port_num << std::endl;
        std::clog << "Inactive clients will be kicked after " << TIMEOUT << " seconds..." << std::endl;
    }

    void send_message(HTTP_request &request, const Client &client) {
        std::uintmax_t file_len = 0;
        std::ifstream f_stream;
        std::string outside_file;

        if (request.code == 200) {
            try {
                f_stream = base_dir.check_existence(request.file);
                file_len = base_dir.size(request.file);
            }
            catch (const file_not_found_exception& e) {
                outside_file = foreign_resources.find(request.file);

                if (outside_file.empty()) {
                    request.code = C_F_NOTFOUND;
                }
                else {
                    request.code = C_F_MOVED;
                }
            }
            // If this matches, then we know that invalid path has been raised, or an exception from the filesystem
            // library.
            catch (...) {
                request.code = C_F_NOTFOUND;
            }
        }

        // Create and send the HTTP start line with associated headers.
        std::string http_no_body = request.create_response(file_len, outside_file);

        client.send_message(http_no_body);

        // If a file was requested, and it exists, then send it's content.
        if (request.code == 200 && request.method == "GET") {
            std::string line;

            client.send_file((base_dir.root_path.string() + request.file).c_str());
        }
    }

    void accept_client() {
        Client client(server_socket.get_socket_num());
        bool conn = true;

        // Get client buffer.
        std::iostream client_stream(&client.get_client_buf());

        std::string line;

        std::clog << "Connection established, waiting for message...\n";

        try {
            while (conn) {
                HTTP_request request;

                while (std::getline(client_stream, line)) {
                    if (line == "\r" || !request.parse_line(line)) {
                        debug("Request has reached its end")
                        debug("Sending message to client")

                        send_message(request, client);

                        break;
                    }
                }

                conn = !request.kill;
            }
        }
        catch (const sigpipe_exception& e) {
            std::cerr << e.what();
        }

        std::clog << "Connection ended, waiting for next clients...\n";
    }

    void runserver() {
        signal(SIGPIPE, SIG_IGN);

        for (;;) {
            accept_client();
        }
    }
};

int main(int argc, char *argv[]) {
    int port_n = PORT_NUM;

    if (argc < 3 || argc > 4) {
        std::cerr << "Invalid arguments!\n Usage: "
                     "./file_name <base directory> <name of co-related server file> [optional] <port number>\n";

        exit(EXIT_FAILURE);
    }

    if (argc == 4) {
        size_t number_size;

        try {
            port_n = std::stoi(argv[3], &number_size);
        }
        catch (...) {
            std::cerr << "Invalid port number format!" << std::endl;

            exit(EXIT_FAILURE);
        }

        if (number_size != std::string(argv[3]).length()) {
            std::cerr << "Invalid port number format!" << std::endl;

            exit(EXIT_FAILURE);
        }

        if (port_n > MAX_PORT_N) {
            std::cerr << "Provided port number is too big!" << std::endl;

            exit(EXIT_FAILURE);
        }
    }

    try {
        TCP_Server server(argv[1], argv[2], port_n);
        server.runserver();
    }
    catch (const base_dir_exception& e) {
        std::cerr << e.what() << std::endl;

        exit(EXIT_FAILURE);
    }
    catch (const corelated_servers_exception& e) {
        std::cerr << e.what() << std::endl;

        exit(EXIT_FAILURE);
    }

    return 0;
}
