#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>
#include <nlohmann/json.hpp>
#include <boost/asio.hpp>
#include <chrono>
#include <future>
#include <memory>
#include <thread>
#include <tuple>

#include "libslic3r/Config.hpp"
#include "libslic3r/PrintConfig.hpp"
#include "slic3r/Utils/BoxPrintMapping.hpp"
#include "slic3r/Utils/Moonraker.hpp"
#include "test_utils.hpp"

using namespace Slic3r;
using nlohmann::json;

namespace {
json box_response()
{
    return {{"result", {{"status", {{"box", {
        {"print_mapping_version", 1}, {"data_ready", true}, {"driver_ready", true},
        {"slots", {{{"index", 8}, {"present", true}, {"external", false}, {"material", "PLA"}},
                   {{"index", 9}, {"present", false}, {"external", false}},
                   {{"index", 12}, {"present", true}, {"external", true}}}}
    }}}}}}};
}

json print_info(const std::string& filename, json tools = {{{"tool", 1}}, {{"tool", 5}}})
{
    return {{"result", {{"status", {{"box", {{"print_info", {
        {"filename", filename}, {"tools", tools}
    }}}}}}}}};
}

// Same loopback HTTP setup as test_box_filament_sync, extended to collect
// complete POST bodies and serve the INFO/query/START sequence.
class PrintServer
{
public:
    struct Reply { unsigned status; std::string body; };
    explicit PrintServer(std::vector<Reply> replies)
        : acceptor(io, {boost::asio::ip::address_v4::loopback(), 0}), socket(io), replies(std::move(replies))
    {
        port = acceptor.local_endpoint().port();
        accept();
        thread = std::thread([this] { io.run(); });
    }
    ~PrintServer() { finish(); }
    void finish()
    {
        io.stop();
        if (thread.joinable())
            thread.join();
    }
    std::string url() const { return "http://127.0.0.1:" + std::to_string(port) + "/moonraker/"; }
    std::vector<std::string> requests;

private:
    void accept()
    {
        acceptor.async_accept(socket, [this](const boost::system::error_code& ec) {
            if (ec)
                return;
            boost::asio::async_read_until(socket, buffer, "\r\n\r\n",
                [this](const boost::system::error_code& ec, std::size_t headers) {
                    if (ec)
                        return;
                    const std::string received(boost::asio::buffers_begin(buffer.data()), boost::asio::buffers_end(buffer.data()));
                    const auto length_header = received.find("Content-Length:");
                    const size_t length = length_header == std::string::npos ? 0 : std::stoul(received.substr(length_header + 15));
                    if (buffer.size() >= headers + length) {
                        respond();
                    } else {
                        boost::asio::async_read(socket, buffer, boost::asio::transfer_exactly(headers + length - buffer.size()),
                            [this](const boost::system::error_code& ec, std::size_t) { if (!ec) respond(); });
                    }
                });
        });
    }
    void respond()
    {
        requests.emplace_back(boost::asio::buffers_begin(buffer.data()), boost::asio::buffers_end(buffer.data()));
        buffer.consume(buffer.size());
        const Reply reply = requests.size() <= replies.size() ? replies[requests.size() - 1] : Reply{500, "unexpected request"};
        response = "HTTP/1.1 " + std::to_string(reply.status) + " Test\r\nContent-Length: " +
                   std::to_string(reply.body.size()) + "\r\nConnection: close\r\n\r\n" + reply.body;
        boost::asio::async_write(socket, boost::asio::buffer(response),
            [this](const boost::system::error_code&, std::size_t) {
                boost::system::error_code ignored;
                socket.close(ignored);
                accept();
            });
    }
    boost::asio::io_context io;
    boost::asio::ip::tcp::acceptor acceptor;
    boost::asio::ip::tcp::socket socket;
    boost::asio::streambuf buffer;
    unsigned short port;
    std::vector<Reply> replies;
    std::string response;
    std::thread thread;
};

json request_body(const std::string& request)
{
    return json::parse(request.substr(request.find("\r\n\r\n") + 4));
}

std::unique_ptr<Moonraker> host(const std::string& url, const std::string& api_key = "")
{
    DynamicPrintConfig config;
    config.set_key_value("print_host", new ConfigOptionString(url));
    config.set_key_value("printhost_apikey", new ConfigOptionString(api_key));
    config.set_key_value("printhost_cafile", new ConfigOptionString(""));
    config.set_key_value("printhost_ssl_ignore_revoke", new ConfigOptionBool(false));
    return std::make_unique<Moonraker>(&config);
}

std::unique_ptr<Moonraker> host(const PrintServer& server, const std::string& api_key = "")
{
    return host(server.url(), api_key);
}
bool legacy_upload(PrintServer& server, PrintHostPostUploadAction action, const std::string& mapping, std::string& error,
                   const PrintHost::ProgressFn& progress = {}, const std::string& storage = {},
                   const std::string& soak = {}, const std::string& print_options = {})
{
    ScopedTemporaryFile source(".gcode");
    {
        boost::filesystem::ofstream file(source.path(), std::ios::binary);
        file << "; filament used [mm] = 0,20,0,0,0,30\nT1\nG1 X10\nT5\n";
    }
    const auto moonraker = host(server, "secret");
    PrintHostUpload upload;
    upload.source_path = source.path();
    upload.upload_path = "requested.gcode";
    upload.post_action = action;
    upload.storage = storage;
    upload.extended_info["box_mapping"] = mapping;
    if (!soak.empty())
        upload.extended_info["heat_soak"] = soak;
    if (!print_options.empty())
        upload.extended_info["print_options"] = print_options;
    return moonraker->upload(std::move(upload), [&](Http::Progress p, bool& cancel) { if (progress) progress(p, cancel); },
                       [&](wxString message) { error = message.ToUTF8().data(); }, [](wxString, wxString) {});
}

const std::string ok = R"({"result":"ok"})";
} // namespace

TEST_CASE("Box mapping keeps sparse tool IDs and raw external slot indices", "[BoxPrintMapping]")
{
    const BoxPrintMapping original{{1, 8}, {5, 12}};
    BoxPrintMapping decoded;
    std::string error;
    REQUIRE(parse_box_print_mapping(serialize_box_print_mapping(original), decoded, error));
    CHECK(decoded == original);
    PrintServer server({{200, box_response().dump()}});
    BoxPrintStatus status;
    REQUIRE(fetch_box_print_status(*host(server, "secret"), status, error));
    server.finish();
    REQUIRE(status.supported);
    REQUIRE(status.slots.size() == 3);
    CHECK(status.slots.back().index == 12);
    CHECK(status.slots.back().external);
    CHECK(status.slots.back().material.empty());
    REQUIRE(server.requests.size() == 1);
    CHECK(server.requests.front().find("X-Api-Key: secret") != std::string::npos);
}

TEST_CASE("Box mapping suggests the slot holding each tool's filament", "[BoxPrintMapping]")
{
    const auto tool = [](int id, const std::string& color, const std::string& material = "PLA") {
        BoxPrintTool tool;
        tool.id = id;
        tool.color = color;
        tool.material = material;
        return tool;
    };
    const auto slot = [](int index, const std::string& color, const std::string& material = "PLA",
                         bool external = false) {
        BoxFilamentSlot slot;
        slot.index = index;
        slot.present = true;
        slot.external = external;
        slot.color = color;
        slot.material = material;
        return slot;
    };

    SECTION("matching filament wins over the tool number")
    {
        CHECK(suggest_box_print_mapping({tool(0, "#ff0000")},
                                        {slot(0, "#000000"), slot(1, "#FFFFFF"), slot(2, "#FF0000")}) ==
              BoxPrintMapping{{0, 2}});
    }
    SECTION("shades, letter case and related materials match")
    {
        CHECK(suggest_box_print_mapping({tool(0, "#FF0000", "PLA"), tool(1, "#000000FF", "petg")},
                                        {slot(0, "#000000"), slot(1, "#C12E1F", "pla+"), slot(2, "#161616", "PETG")}) ==
              BoxPrintMapping{{0, 1}, {1, 2}});
    }
    SECTION("the exact material beats a related one")
    {
        CHECK(suggest_box_print_mapping({tool(0, "#FF0000")}, {slot(0, "#FF0000", "PLA-CF"), slot(1, "#FF0000")}) ==
              BoxPrintMapping{{0, 1}});
    }
    SECTION("no match keeps the tool's own slot when offered")
    {
        CHECK(suggest_box_print_mapping({tool(2, "#C12E1F"), tool(3, "#C12E1F")},
                                        {slot(0, "#FF6A13"), slot(1, "#C12E1F", "PETG"), slot(2, "#0000FF")}) ==
              BoxPrintMapping{{2, 2}});
    }
    SECTION("related materials written with spaces match")
    {
        CHECK(suggest_box_print_mapping({tool(0, " #FF0000\r\n")}, {slot(0, "#000000"), slot(1, "#FF0000", "PLA Matte")}) ==
              BoxPrintMapping{{0, 1}});
    }
    SECTION("identical slots are spread and a single match is shared")
    {
        const std::vector<BoxFilamentSlot> slots{slot(0, "#FF0000"), slot(1, "#FF0000"), slot(2, "#0000FF")};
        CHECK(suggest_box_print_mapping({tool(0, "#FF0000"), tool(1, "#FF0000")}, slots) ==
              BoxPrintMapping{{0, 0}, {1, 1}});
        CHECK(suggest_box_print_mapping({tool(0, "#0000FF"), tool(1, "#0000FF")}, slots) ==
              BoxPrintMapping{{0, 2}, {1, 2}});
    }
    SECTION("ties go to the tool number, then Box slots before External")
    {
        CHECK(suggest_box_print_mapping({tool(1, "#FF0000"), tool(2, "#00FF00")},
                                        {slot(0, "#FF0000"), slot(1, "#FF0000"), slot(4, "#00FF00", "PLA", true),
                                         slot(5, "#00FF00")}) == BoxPrintMapping{{1, 1}, {2, 5}});
    }
}

TEST_CASE("Box status probe can run without blocking the caller", "[BoxPrintMapping]")
{
    PrintServer server({{200, box_response().dump()}});
    std::promise<std::tuple<bool, BoxPrintStatus, std::string>> result;
    fetch_box_print_status_async(*host(server, "secret"), [&](bool fetched, const BoxPrintStatus& status, const std::string& error) {
        result.set_value({fetched, status, error});
    });
    auto future = result.get_future();
    REQUIRE(future.wait_for(std::chrono::seconds(10)) == std::future_status::ready);
    const auto [fetched, status, error] = future.get();
    server.finish();
    CHECK(fetched);
    CHECK(status.supported);
    CHECK(status.slots.size() == 3);
    CHECK(error.empty());
    REQUIRE(server.requests.size() == 1);
    CHECK(server.requests.front().find("X-Api-Key: secret") != std::string::npos);
}

TEST_CASE("Box status probe reports unsupported and failed hosts asynchronously", "[BoxPrintMapping]")
{
    const unsigned http_status = GENERATE(200u, 404u, 500u);
    PrintServer server({{http_status, http_status == 200 ? R"({"result":{"status":{}}})" : R"({"error":{"message":"x"}})"}});
    std::promise<std::pair<bool, bool>> result;
    fetch_box_print_status_async(*host(server), [&](bool fetched, const BoxPrintStatus& status, const std::string&) {
        result.set_value({fetched, status.supported});
    });
    auto future = result.get_future();
    REQUIRE(future.wait_for(std::chrono::seconds(10)) == std::future_status::ready);
    const auto [fetched, supported] = future.get();
    server.finish();
    CHECK(fetched == (http_status != 500));
    CHECK_FALSE(supported);
}

TEST_CASE("Box mapping rejects invalid serialized selections", "[BoxPrintMapping]")
{
    const auto value = GENERATE("{}", "[]", "null", "bad", R"({"01":8})", R"({"-1":8})",
                                R"({"256":8})", R"({"1":-1})", R"({"1":2.5})", R"({"1":"8"})",
                                R"({"1":4294967296})", R"({"2147483648":8})");
    BoxPrintMapping mapping{{1, 8}};
    std::string error;
    CHECK_FALSE(parse_box_print_mapping(value, mapping, error));
    CHECK(mapping.empty());
    CHECK_FALSE(error.empty());
}

TEST_CASE("Box mapped printing inspects the stored filename before starting its exact map", "[BoxPrintMapping]")
{
    auto status = box_response();
    auto& box = status["result"]["status"]["box"];
    const int revision = GENERATE(0, 2, 3);
    if (revision != 0) {
        box.erase("print_mapping_version");
        box["api_version"] = 1;
        box["api_revision"] = revision;
    }
    const std::string filename = "nested/renamed \"quoted\" \\ part.gcode";
    PrintServer server({{200, status.dump()}, {200, ok}, {200, print_info(filename).dump()}, {200, ok}});
    std::string error;
    REQUIRE(start_box_print(*host(server, "secret"), filename, R"({"1":8,"5":12})", error));
    server.finish();
    REQUIRE(server.requests.size() == 4);
    CHECK(server.requests[0].find("GET /moonraker/printer/objects/query?box HTTP/") == 0);
    CHECK(request_body(server.requests[1])["script"] == "BOX_PRINT_INFO FILENAME=\"nested/renamed \\\"quoted\\\" \\\\ part.gcode\"");
    CHECK(server.requests[2].find("GET /moonraker/printer/objects/query?box=print_info HTTP/") == 0);
    CHECK(request_body(server.requests[3])["script"] == "BOX_PRINT_START FILENAME=\"nested/renamed \\\"quoted\\\" \\\\ part.gcode\" MAP=\"1:8,5:12\"");
}

TEST_CASE("Box mapping accepts either supported capability advertisement", "[BoxPrintMapping]")
{
    auto response = box_response();
    auto& box = response["result"]["status"]["box"];
    const bool legacy = GENERATE(false, true);
    box["print_mapping_version"] = legacy ? 1 : 2;
    box["api_version"] = legacy ? 2 : 1;
    box["api_revision"] = 2;
    PrintServer server({{200, response.dump()}});
    BoxPrintStatus status;
    std::string error;
    REQUIRE(fetch_box_print_status(*host(server), status, error));
    server.finish();
    CHECK(status.supported);
    CHECK(status.slots.size() == 3);
}

TEST_CASE("Box mapping requires a supported status API version and integer revision", "[BoxPrintMapping]")
{
    auto response = box_response();
    auto& box = response["result"]["status"]["box"];
    box.erase("print_mapping_version");
    const auto version = GENERATE(std::string(R"({})"),
        std::string(R"({"api_version":1,"api_revision":1})"),
        std::string(R"({"api_version":2,"api_revision":2})"),
        std::string(R"({"api_version":1})"), std::string(R"({"api_revision":2})"),
        std::string(R"({"api_version":"1","api_revision":2})"),
        std::string(R"({"api_version":1,"api_revision":"2"})"),
        std::string(R"({"api_version":1,"api_revision":2.0})"),
        std::string(R"({"api_version":true,"api_revision":2})"),
        std::string(R"({"api_version":1,"api_revision":true})"));
    box.update(json::parse(version));
    PrintServer server({{200, response.dump()}});
    BoxPrintStatus status;
    std::string error;
    REQUIRE(fetch_box_print_status(*host(server), status, error));
    server.finish();
    CHECK_FALSE(status.supported);
    CHECK(status.slots.empty());
    CHECK(error.empty());
}

TEST_CASE("Revision two Box mapping still requires ready valid slots", "[BoxPrintMapping]")
{
    auto response = box_response();
    auto& box = response["result"]["status"]["box"];
    box.erase("print_mapping_version");
    box["api_version"] = 1;
    box["api_revision"] = 2;
    const int invalid = GENERATE(0, 1, 2);
    if (invalid == 0)
        box["data_ready"] = false;
    else if (invalid == 1)
        box["driver_ready"] = false;
    else
        box["slots"] = json::array();
    PrintServer server({{200, response.dump()}});
    std::string error;
    CHECK_FALSE(start_box_print(*host(server), "stored.gcode", R"({"1":8,"5":12})", error));
    server.finish();
    CHECK(server.requests.size() == 1);
    CHECK_FALSE(error.empty());
}

TEST_CASE("Box print refuses changed or invalid file metadata without ordinary fallback", "[BoxPrintMapping]")
{
    auto info = print_info("stored.gcode");
    auto& data = info["result"]["status"]["box"]["print_info"];
    const int scenario = GENERATE(0, 1, 2, 3, 4, 5, 6);
    switch (scenario) {
    case 0: data["filename"] = "other.gcode"; break;
    case 1: data["tools"] = json::object(); break;
    case 2: data["tools"] = json::array(); break;
    case 3: data["tools"] = {{{"tool", 0}}, {{"tool", 5}}}; break;
    case 4: data["tools"] = {{{"tool", 1}}, {{"tool", 5}}, {{"tool", 6}}}; break;
    case 5: data["tools"] = {{{"tool", 1}}, {{"tool", 1}}}; break;
    case 6: data["tools"] = {{{"tool", "1"}}, {{"tool", 5}}}; break;
    }
    PrintServer server({{200, box_response().dump()}, {200, ok}, {200, info.dump()}});
    std::string error;
    CHECK_FALSE(start_box_print(*host(server), "stored.gcode", R"({"1":8,"5":12})", error));
    server.finish();
    CHECK(server.requests.size() == 3);
    CHECK_FALSE(error.empty());
}

TEST_CASE("Box print never ignores a selected map when firmware becomes unsupported", "[BoxPrintMapping]")
{
    PrintServer server({{200, R"({"result":{"status":{}}})"}});
    std::string error;
    CHECK_FALSE(start_box_print(*host(server), "stored.gcode", R"({"1":8})", error));
    server.finish();
    CHECK(server.requests.size() == 1);
}

TEST_CASE("Ordinary printing remains available without Box mapping support", "[BoxPrintMapping]")
{
    const auto response = GENERATE(std::string(R"({"result":{"status":{}}})"),
                                  std::string(R"({"result":{"status":{"box":{"print_mapping_version":2}}}})"));
    PrintServer server({{200, response}, {200, ok}});
    std::string error;
    REQUIRE(start_box_print(*host(server), "stored.gcode", "", error));
    server.finish();
    REQUIRE(server.requests.size() == 2);
    CHECK(server.requests.back().find("POST /moonraker/printer/print/start HTTP/") == 0);
    CHECK(request_body(server.requests.back())["filename"] == "stored.gcode");
}

TEST_CASE("Box permits ordinary print for missing usage metadata only without a selection", "[BoxPrintMapping]")
{
    PrintServer server({{200, box_response().dump()}, {200, ok},
                        {200, print_info("stored.gcode", json::array()).dump()}, {200, ok}});
    std::string error;
    REQUIRE(start_box_print(*host(server), "stored.gcode", "", error));
    server.finish();
    REQUIRE(server.requests.size() == 4);
    CHECK(server.requests.back().find("POST /moonraker/printer/print/start HTTP/") == 0);
}

TEST_CASE("Box start errors and failed inspection never trigger an ordinary start", "[BoxPrintMapping]")
{
    const int failure = GENERATE(1, 2, 3);
    std::vector<PrintServer::Reply> replies{{200, box_response().dump()}, {200, ok},
                                         {200, print_info("stored.gcode").dump()}, {200, ok}};
    replies[failure] = {200, R"({"error":{"message":"File changed"}})"};
    replies.resize(failure + 1);
    PrintServer server(std::move(replies));
    std::string error;
    CHECK_FALSE(start_box_print(*host(server), "stored.gcode", R"({"1":8,"5":12})", error));
    server.finish();
    CHECK(server.requests.size() == failure + 1);
    CHECK(error.find("File changed") != std::string::npos);
}

TEST_CASE("Box supported but unready status blocks printing", "[BoxPrintMapping]")
{
    auto response = box_response();
    auto& box = response["result"]["status"]["box"];
    const int scenario = GENERATE(0, 1);
    switch (scenario) {
    case 0: box["driver_ready"] = false; break;
    case 1: box["data_ready"] = false; break;
    }
    PrintServer server({{200, response.dump()}});
    std::string error;
    CHECK_FALSE(start_box_print(*host(server), "stored.gcode", R"({"1":8,"5":12})", error));
    server.finish();
    CHECK(server.requests.size() == 1);
}

TEST_CASE("Box authentication and server errors block rather than bypass mapping", "[BoxPrintMapping]")
{
    const unsigned status = GENERATE(401u, 500u);
    PrintServer server({{status, R"({"error":{"message":"Unavailable"}})"}});
    BoxPrintStatus box;
    std::string error;
    CHECK_FALSE(fetch_box_print_status(*host(server), box, error));
    server.finish();
    CHECK_FALSE(error.empty());
}

TEST_CASE("Box cancellation after inspection prevents print start", "[BoxPrintMapping]")
{
    PrintServer server({{200, box_response().dump()}, {200, ok}, {200, print_info("stored.gcode").dump()}});
    unsigned checks = 0;
    std::string error;
    CHECK_FALSE(start_box_print(*host(server), "stored.gcode", R"({"1":8,"5":12})", error,
                                [&] { return ++checks == 4; }));
    server.finish();
    CHECK(server.requests.size() == 3);
    CHECK(error == "Box print cancelled.");
}

TEST_CASE("Box print rejects command separators before contacting the printer", "[BoxPrintMapping]")
{
    const auto filename = GENERATE(std::string("file.gcode\nM112"), std::string("file.gcode\rM112"), std::string("a\0b", 3));
    std::string error;
    CHECK_FALSE(start_box_print(*host("http://127.0.0.1:1"), filename, R"({"1":8})", error));
    CHECK(error.find("line break or null") != std::string::npos);
}

TEST_CASE("Box absence on older Moonraker compatible hosts preserves ordinary printing", "[BoxPrintMapping]")
{
    const unsigned status = GENERATE(404u, 501u);
    PrintServer server({{status, R"({"error":{"message":"Not implemented"}})"}, {200, ok}});
    std::string error;
    REQUIRE(start_box_print(*host(server), "stored.gcode", "", error));
    server.finish();
    REQUIRE(server.requests.size() == 2);
    CHECK(server.requests.back().find("POST /moonraker/printer/print/start HTTP/") == 0);
}

TEST_CASE("Box tools discovered after upload require an explicit selection", "[BoxPrintMapping]")
{
    PrintServer server({{200, box_response().dump()}, {200, ok}, {200, print_info("stored.gcode").dump()}});
    std::string error;
    CHECK_FALSE(start_box_print(*host(server), "stored.gcode", "", error));
    server.finish();
    CHECK(server.requests.size() == 3);
    CHECK_FALSE(error.empty());
}

TEST_CASE("Malformed Box status never masquerades as unsupported firmware", "[BoxPrintMapping]")
{
    const auto response = GENERATE(std::string("not JSON"), std::string("null"),
                                  std::string(R"({"result":{"status":{"box":{"print_mapping_version":"1"}}}})"),
                                  std::string(R"({"result":{"status":{"box":{"print_mapping_version":1}}}})"));
    PrintServer server({{200, response}});
    BoxPrintStatus status;
    std::string error;
    CHECK_FALSE(fetch_box_print_status(*host(server), status, error));
    server.finish();
    CHECK(status.slots.empty());
    CHECK_FALSE(error.empty());
}

TEST_CASE("Legacy Moonraker upload only leaves Box mapping and printing untouched", "[BoxPrintMapping]")
{
    PrintServer server({{200, R"({"result":{"klippy_state":"ready"}})"},
                        {201, R"({"item":{"path":"stored.gcode","root":"gcodes"},"action":"create_file","print_started":false,"print_queued":false})"}});
    std::string error;
    bool completed = false;
    REQUIRE(legacy_upload(server, PrintHostPostUploadAction::None, R"({"1":8,"5":12})", error,
                          [&](Http::Progress progress, bool&) {
                              if (progress.ultotal > 0 && progress.ulnow == progress.ultotal)
                                  completed = true;
                          }));
    server.finish();
    REQUIRE(server.requests.size() == 2);
    CHECK(server.requests[1].find("POST /moonraker/server/files/upload HTTP/") == 0);
    CHECK(server.requests[1].find("; filament used [mm] = 0,20,0,0,0,30\nT1\nG1 X10\nT5\n") != std::string::npos);
    CHECK(server.requests[1].find("box_mapping") == std::string::npos);
    CHECK(completed);
}

TEST_CASE("Legacy Moonraker maps the actual server stored filename after upload", "[BoxPrintMapping]")
{
    const std::string filename = "nested/renamed.gcode";
    PrintServer server({{200, R"({"result":{"klippy_state":"ready"}})"},
                        {201, json{{"item", {{"path", filename}, {"root", "gcodes"}}},
                                   {"action", "create_file"}, {"print_started", false}, {"print_queued", false}}.dump()},
                        {200, box_response().dump()}, {200, ok}, {200, print_info(filename).dump()}, {200, ok}});
    std::string error;
    REQUIRE(legacy_upload(server, PrintHostPostUploadAction::StartPrint, R"({"1":8,"5":12})", error));
    server.finish();
    REQUIRE(server.requests.size() == 6);
    CHECK(server.requests[1].find("requested.gcode") != std::string::npos);
    CHECK(request_body(server.requests[3])["script"] == "BOX_PRINT_INFO FILENAME=\"nested/renamed.gcode\"");
    CHECK(request_body(server.requests[5])["script"] ==
          "BOX_PRINT_START FILENAME=\"nested/renamed.gcode\" MAP=\"1:8,5:12\"");
}

TEST_CASE("Legacy printing without a Box mapping lets Moonraker start the upload", "[BoxPrintMapping]")
{
    const std::string flag = GENERATE(std::string("print_started"), std::string("print_queued"));
    PrintServer server({{200, R"({"result":{"klippy_state":"ready"}})"},
                        {201, json{{"item", {{"path", "stored.gcode"}, {"root", "gcodes"}}}, {"action", "create_file"},
                                   {"print_started", flag == "print_started"}, {"print_queued", flag == "print_queued"}}.dump()}});
    std::string error;
    REQUIRE(legacy_upload(server, PrintHostPostUploadAction::StartPrint, "", error));
    server.finish();
    REQUIRE(server.requests.size() == 2);
    CHECK(server.requests[1].find("name=\"print\"\r\n\r\ntrue") != std::string::npos);
}

TEST_CASE("Legacy printing falls back to an explicit start when Moonraker does not start the upload", "[BoxPrintMapping]")
{
    PrintServer server({{200, R"({"result":{"klippy_state":"ready"}})"},
                        {201, R"({"item":{"path":"stored.gcode","root":"gcodes"},"print_started":false,"print_queued":false})"},
                        {200, R"({"result":{"status":{}}})"}, {200, ok}});
    std::string error;
    REQUIRE(legacy_upload(server, PrintHostPostUploadAction::StartPrint, "", error));
    server.finish();
    REQUIRE(server.requests.size() == 4);
    CHECK(server.requests[3].find("POST /moonraker/printer/print/start HTTP/") == 0);
    CHECK(request_body(server.requests[3])["filename"] == "stored.gcode");
}

TEST_CASE("Legacy mapped printing never asks Moonraker to start the upload", "[BoxPrintMapping]")
{
    // A server claiming it queued the print is ignored: the file still goes through Box.
    PrintServer server({{200, R"({"result":{"klippy_state":"ready"}})"},
                        {201, R"({"item":{"path":"stored.gcode","root":"gcodes"},"print_started":false,"print_queued":true})"},
                        {200, box_response().dump()}, {200, ok}, {200, print_info("stored.gcode").dump()}, {200, ok}});
    std::string error;
    REQUIRE(legacy_upload(server, PrintHostPostUploadAction::StartPrint, R"({"1":8,"5":12})", error));
    server.finish();
    REQUIRE(server.requests.size() == 6);
    CHECK(server.requests[1].find("name=\"print\"\r\n\r\nfalse") != std::string::npos);
    CHECK(request_body(server.requests[5])["script"] == "BOX_PRINT_START FILENAME=\"stored.gcode\" MAP=\"1:8,5:12\"");
}

TEST_CASE("Legacy mapped printing requires the upload response to confirm the filename", "[BoxPrintMapping]")
{
    PrintServer server({{200, R"({"result":{"klippy_state":"ready"}})"}, {200, ok}});
    std::string error;
    CHECK_FALSE(legacy_upload(server, PrintHostPostUploadAction::StartPrint, R"({"1":8,"5":12})", error));
    server.finish();
    CHECK(server.requests.size() == 2);
    CHECK(error.find("did not confirm") != std::string::npos);
}

TEST_CASE("Legacy print jobs stay cancellable until the print start request is sent", "[BoxPrintMapping]")
{
    const bool start_succeeds = GENERATE(true, false);
    PrintServer server({{200, R"({"result":{"klippy_state":"ready"}})"},
                        {201, R"({"item":{"path":"stored.gcode","root":"gcodes"},"action":"create_file","print_started":false,"print_queued":false})"},
                        {200, box_response().dump()}, {200, ok}, {200, print_info("stored.gcode").dump()},
                        {200, start_succeeds ? ok : R"({"error":{"message":"File changed"}})"}});
    std::vector<size_t> percentages;
    std::string error;
    const bool result = legacy_upload(server, PrintHostPostUploadAction::StartPrint, R"({"1":8,"5":12})", error,
                                     [&](Http::Progress progress, bool&) {
                                         percentages.push_back(progress.ultotal > 0 ? 100 * progress.ulnow / progress.ultotal : 0);
                                     });
    server.finish();
    CHECK(result == start_succeeds);
    REQUIRE(server.requests.size() == 6);
    REQUIRE(percentages.size() > 1);
    // Only the report sent just before the start request completes the job.
    CHECK(percentages.back() == 100);
    for (size_t i = 0; i + 1 < percentages.size(); ++i)
        CHECK(percentages[i] < 100);
}

TEST_CASE("Legacy print cancelled as the start request is sent never starts", "[BoxPrintMapping]")
{
    PrintServer server({{200, R"({"result":{"klippy_state":"ready"}})"},
                        {201, R"({"item":{"path":"stored.gcode","root":"gcodes"},"print_started":false})"},
                        {200, box_response().dump()}, {200, ok}, {200, print_info("stored.gcode").dump()}});
    std::string error;
    bool cancel_seen = false;
    CHECK_FALSE(legacy_upload(server, PrintHostPostUploadAction::StartPrint, R"({"1":8,"5":12})", error,
                              [&](Http::Progress progress, bool& cancel) {
                                  if (cancel)
                                      cancel_seen = true;
                                  else if (progress.ultotal > 0 && progress.ulnow == progress.ultotal && !cancel_seen)
                                      cancel = true;
                              }));
    server.finish();
    CHECK(server.requests.size() == 5);
    CHECK(cancel_seen);
    CHECK(error.empty());
}

TEST_CASE("Legacy printing requires the gcodes storage root", "[BoxPrintMapping]")
{
    SECTION("another storage is rejected before upload")
    {
        PrintServer server({{200, R"({"result":{"klippy_state":"ready"}})"}});
        std::string error;
        CHECK_FALSE(legacy_upload(server, PrintHostPostUploadAction::StartPrint, R"({"1":8,"5":12})", error, {}, "config"));
        server.finish();
        CHECK(server.requests.size() == 1);
        CHECK(error.find("requires the gcodes storage") != std::string::npos);
    }
    SECTION("a file stored outside gcodes is not started")
    {
        PrintServer server({{200, R"({"result":{"klippy_state":"ready"}})"},
                            {201, R"({"item":{"path":"stored.gcode","root":"config"},"print_started":false})"}});
        std::string error;
        CHECK_FALSE(legacy_upload(server, PrintHostPostUploadAction::StartPrint, "", error));
        server.finish();
        CHECK(server.requests.size() == 2);
        CHECK(error.find("outside gcodes") != std::string::npos);
    }
}

TEST_CASE("Legacy mapped printing reports firmware rejection when a selected slot becomes empty", "[BoxPrintMapping]")
{
    auto status = box_response();
    status["result"]["status"]["box"]["slots"][0]["present"] = false;
    PrintServer server({{200, R"({"result":{"klippy_state":"ready"}})"},
                        {201, R"({"item":{"path":"stored.gcode","root":"gcodes"},"print_started":false})"},
                        {200, status.dump()}, {200, ok}, {200, print_info("stored.gcode").dump()},
                        {400, R"({"error":{"code":400,"message":"[BOX]: Slot T8 has no filament"}})"}});
    std::string error;
    CHECK_FALSE(legacy_upload(server, PrintHostPostUploadAction::StartPrint, R"({"1":8,"5":12})", error));
    server.finish();
    REQUIRE(server.requests.size() == 6);
    CHECK(request_body(server.requests.back())["script"] ==
          "BOX_PRINT_START FILENAME=\"stored.gcode\" MAP=\"1:8,5:12\"");
    CHECK(error.find("[BOX]: Slot T8 has no filament") != std::string::npos);
}

namespace {
PrintStartOptions print_start_options(PrintServer& server)
{
    std::promise<PrintStartOptions> result;
    fetch_print_start_options_async(*host(server), [&](const PrintStartOptions& options) { result.set_value(options); });
    auto future = result.get_future();
    REQUIRE(future.wait_for(std::chrono::seconds(10)) == std::future_status::ready);
    server.finish();
    return future.get();
}

// What Klipper answers: {} for an object the printer does not have, and null
// for a queried variable the macro lacks.
json start_options_reply(json vars, json control = json::object())
{
    return {{"result", {{"status", {{"print_control", std::move(control)}, {"gcode_macro _START_PRINT_VARS", std::move(vars)}}}}}};
}

const json soak_option = {{"id", "soak"}, {"label", "Heat soak"}, {"section", "Before printing"}, {"hint", ""},
                          {"type", "minutes"}, {"optional", false}, {"value_label", ""}, {"min", 0}, {"max", 240},
                          {"step", 5}, {"default", 7.6}};
const json keep_option = {{"id", "keep_heated"}, {"label", "Keep bed and chamber heated"}, {"section", "After printing"},
                          {"hint", "The nozzle still turns off."}, {"type", "minutes"}, {"optional", true},
                          {"value_label", "Turn off after"}, {"min", 1}, {"max", 240}, {"step", 15}, {"default", 30}};
const json switch_option = {{"id", "pa_calibrate"}, {"label", "Calibrate pressure advance"}, {"section", "Before printing"},
                            {"hint", ""}, {"type", "switch"}, {"default", false}};

json print_control(json options) { return {{"version", 1}, {"options", std::move(options)}, {"active", json::object()}}; }
} // namespace

TEST_CASE("The printer's print options are offered as it describes them", "[BoxPrintMapping]")
{
    PrintServer server({{200, start_options_reply({{"heat_soak", 5}, {"soak_once", -1}},
                                                  print_control({soak_option, keep_option, switch_option})).dump()}});
    const auto offered = print_start_options(server);
    REQUIRE(server.requests.size() == 1);
    CHECK(server.requests.front().find(
              "GET /moonraker/printer/objects/query?print_control&gcode_macro%20_START_PRINT_VARS=heat_soak,soak_once HTTP/") == 0);
    // Print options replace the one-time soak.
    CHECK_FALSE(offered.soak_once_default);
    REQUIRE(offered.options.size() == 3);
    const auto& soak = offered.options[0];
    CHECK(soak.id == "soak");
    CHECK(soak.label == "Heat soak");
    CHECK(soak.section == "Before printing");
    CHECK(soak.type == PrintOption::Type::Minutes);
    CHECK_FALSE(soak.optional);
    CHECK(soak.min == 0);
    CHECK(soak.max == 240);
    CHECK(soak.step == 5);
    CHECK(soak.default_value == 8);
    const auto& keep = offered.options[1];
    CHECK(keep.optional);
    CHECK(keep.hint == "The nozzle still turns off.");
    CHECK(keep.value_label == "Turn off after");
    CHECK(keep.min == 1);
    CHECK(keep.step == 15);
    CHECK(keep.default_value == 30);
    CHECK(offered.options[2].type == PrintOption::Type::Switch);
    CHECK(offered.options[2].default_value == 0);
}

TEST_CASE("Print options the dialog cannot draw are left out and values kept in range", "[BoxPrintMapping]")
{
    json unknown_type = soak_option, bad_id = soak_option, no_label = soak_option, wild = keep_option, bare;
    unknown_type["type"] = "choice";
    bad_id["id"]         = "Bad Id";
    no_label["label"]    = "";
    wild["default"]      = 1e12;
    wild["step"]         = 0;
    bare                 = {{"id", "bare"}, {"label", "Bare"}, {"type", "minutes"}};
    PrintServer server({{200, start_options_reply({{"heat_soak", nullptr}, {"soak_once", nullptr}},
                                                  print_control({unknown_type, bad_id, no_label, nullptr, wild, bare})).dump()}});
    const auto offered = print_start_options(server);
    REQUIRE(offered.options.size() == 2);
    CHECK(offered.options[0].id == "keep_heated");
    CHECK(offered.options[0].default_value == 240);
    CHECK(offered.options[0].step == 1);
    CHECK(offered.options[1].id == "bare");
    CHECK(offered.options[1].min == 0);
    CHECK(offered.options[1].max == 240);
    CHECK(offered.options[1].default_value == 0);
}

TEST_CASE("Print choices send only what was changed or switched on", "[BoxPrintMapping]")
{
    PrintServer server({{200, start_options_reply({{"heat_soak", 5}, {"soak_once", -1}},
                                                  print_control({soak_option, keep_option, switch_option})).dump()}});
    const auto options = print_start_options(server).options;
    REQUIRE(options.size() == 3);
    std::vector<PrintChoice> choices;
    for (const auto& option : options)
        choices.push_back(default_print_choice(option));
    CHECK(choices[0].on);
    CHECK(choices[0].minutes == 8);
    CHECK_FALSE(choices[1].on);
    CHECK(choices[1].minutes == 30);
    CHECK_FALSE(choices[2].on);
    CHECK(print_options_command(options, choices).empty());

    choices[1].on = true;
    CHECK(print_options_command(options, choices) == "PRINT_OPTIONS KEEP_HEATED=30");
    choices[0].minutes = 0;
    choices[1].minutes = 999;
    choices[2].on      = true;
    CHECK(print_options_command(options, choices) == "PRINT_OPTIONS SOAK=0 KEEP_HEATED=240 PA_CALIBRATE=1");
}

TEST_CASE("The one-time heat soak is offered when the printer has no print options", "[BoxPrintMapping]")
{
    PrintServer server({{200, start_options_reply({{"heat_soak", 7.6}, {"soak_once", -1}}).dump()}});
    const auto offered = print_start_options(server);
    CHECK(offered.options.empty());
    REQUIRE(offered.soak_once_default);
    CHECK(*offered.soak_once_default == 8);
}

TEST_CASE("Nothing is offered for older configs, other printers and failed queries", "[BoxPrintMapping]")
{
    const auto reply = GENERATE(
        // What Klipper answers for a config without soak_once.
        PrintServer::Reply{200, start_options_reply({{"heat_soak", 5}, {"soak_once", nullptr}}).dump()},
        PrintServer::Reply{200, start_options_reply({{"heat_soak", 5}}).dump()},
        PrintServer::Reply{200, start_options_reply({{"heat_soak", "5"}, {"soak_once", -1}}).dump()},
        PrintServer::Reply{200, start_options_reply({{"heat_soak", -1}, {"soak_once", -1}}).dump()},
        // Longer than the dialog offers: left to the printer's setting.
        PrintServer::Reply{200, start_options_reply({{"heat_soak", MAX_HEAT_SOAK_MINUTES + 1}, {"soak_once", -1}}).dump()},
        // print_control without a version or an option list.
        PrintServer::Reply{200, start_options_reply({{"heat_soak", nullptr}, {"soak_once", nullptr}},
                                                    {{"options", json::array({soak_option})}}).dump()},
        PrintServer::Reply{200, start_options_reply({{"heat_soak", nullptr}, {"soak_once", nullptr}}, {{"version", 1}}).dump()},
        PrintServer::Reply{200, R"({"result":{"status":{}}})"},
        PrintServer::Reply{200, R"({"result":[]})"},
        PrintServer::Reply{200, "not JSON"},
        PrintServer::Reply{404, R"({"error":{"message":"x"}})"});
    PrintServer server({reply});
    const auto offered = print_start_options(server);
    CHECK(offered.options.empty());
    CHECK_FALSE(offered.soak_once_default);
}

TEST_CASE("Print options go ahead of a Box start in the same request", "[BoxPrintMapping]")
{
    PrintServer server({{200, box_response().dump()}, {200, ok}, {200, print_info("stored.gcode").dump()}, {200, ok}});
    std::string error;
    PrintStartChoices choices;
    choices.print_options = "PRINT_OPTIONS SOAK=0 KEEP_HEATED=30";
    REQUIRE(start_box_print(*host(server), "stored.gcode", R"({"1":8,"5":12})", error, {}, {}, choices));
    server.finish();
    REQUIRE(server.requests.size() == 4);
    CHECK(request_body(server.requests[3])["script"] ==
          "PRINT_OPTIONS SOAK=0 KEEP_HEATED=30\n"
          "BOX_PRINT_START FILENAME=\"stored.gcode\" MAP=\"1:8,5:12\"");
}

TEST_CASE("A chosen one-time heat soak is set in the same request as a Box start", "[BoxPrintMapping]")
{
    PrintServer server({{200, box_response().dump()}, {200, ok}, {200, print_info("stored.gcode").dump()}, {200, ok}});
    std::string error;
    PrintStartChoices choices;
    choices.soak_once = 0;
    REQUIRE(start_box_print(*host(server), "stored.gcode", R"({"1":8,"5":12})", error, {}, {}, choices));
    server.finish();
    REQUIRE(server.requests.size() == 4);
    CHECK(request_body(server.requests[3])["script"] ==
          "BOX_PRINT_START FILENAME=\"stored.gcode\" MAP=\"1:8,5:12\"\n"
          "SET_GCODE_VARIABLE MACRO=_START_PRINT_VARS VARIABLE=soak_once VALUE=0");
}

TEST_CASE("A chosen one-time heat soak replaces Moonraker's start with one start script", "[BoxPrintMapping]")
{
    PrintServer server({{200, R"({"result":{"status":{}}})"}, {200, ok}});
    std::string error;
    PrintStartChoices choices;
    choices.soak_once = 12;
    REQUIRE(start_box_print(*host(server), "a \"b\".gcode", "", error, {}, {}, choices));
    server.finish();
    REQUIRE(server.requests.size() == 2);
    CHECK(server.requests[1].find("POST /moonraker/printer/gcode/script HTTP/") == 0);
    CHECK(request_body(server.requests[1])["script"] ==
          "SDCARD_PRINT_FILE FILENAME=\"a \\\"b\\\".gcode\"\n"
          "SET_GCODE_VARIABLE MACRO=_START_PRINT_VARS VARIABLE=soak_once VALUE=12");
}

TEST_CASE("Upload and Print with a chosen one-time heat soak starts the stored file itself", "[BoxPrintMapping]")
{
    PrintServer server({{200, R"({"result":{"klippy_state":"ready"}})"},
                        {201, json{{"item", {{"path", "stored.gcode"}, {"root", "gcodes"}}}, {"action", "create_file"},
                                   {"print_started", false}, {"print_queued", false}}.dump()},
                        {200, R"({"result":{"status":{}}})"}, {200, ok}});
    std::string error;
    REQUIRE(legacy_upload(server, PrintHostPostUploadAction::StartPrint, "", error, {}, {}, "15"));
    server.finish();
    REQUIRE(server.requests.size() == 4);
    CHECK(server.requests[1].find("name=\"print\"\r\n\r\nfalse") != std::string::npos);
    CHECK(request_body(server.requests[3])["script"] ==
          "SDCARD_PRINT_FILE FILENAME=\"stored.gcode\"\n"
          "SET_GCODE_VARIABLE MACRO=_START_PRINT_VARS VARIABLE=soak_once VALUE=15");
}

TEST_CASE("Upload and Print with print options starts the stored file itself", "[BoxPrintMapping]")
{
    PrintServer server({{200, R"({"result":{"klippy_state":"ready"}})"},
                        {201, json{{"item", {{"path", "stored.gcode"}, {"root", "gcodes"}}}, {"action", "create_file"},
                                   {"print_started", false}, {"print_queued", false}}.dump()},
                        {200, R"({"result":{"status":{}}})"}, {200, ok}});
    std::string error;
    REQUIRE(legacy_upload(server, PrintHostPostUploadAction::StartPrint, "", error, {}, {}, {}, "PRINT_OPTIONS KEEP_HEATED=45"));
    server.finish();
    REQUIRE(server.requests.size() == 4);
    CHECK(server.requests[1].find("name=\"print\"\r\n\r\nfalse") != std::string::npos);
    CHECK(request_body(server.requests[3])["script"] ==
          "PRINT_OPTIONS KEEP_HEATED=45\n"
          "SDCARD_PRINT_FILE FILENAME=\"stored.gcode\"");
}

TEST_CASE("Box material warnings flag only different material families", "[BoxPrintMapping]")
{
    CHECK(box_materials_conflict("PLA", "PA6-GF"));
    CHECK_FALSE(box_materials_conflict("PLA", "PLA+"));
    CHECK_FALSE(box_materials_conflict("pla", "PLA"));
    CHECK_FALSE(box_materials_conflict("PLA", ""));
}
