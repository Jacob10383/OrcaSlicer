#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>
#include <nlohmann/json.hpp>
#include <boost/asio.hpp>
#include <thread>

#include "slic3r/Utils/BoxFilamentSync.hpp"

using namespace Slic3r;
using nlohmann::json;

namespace {

json slot(int index, bool present = true, bool external = false)
{
    return {{"index", index}, {"present", present}, {"loaded", false}, {"external", external},
            {"material", "PLA"}, {"color", "#12AB34"}, {"brand", "Creality"}, {"name", "My tuned PLA"}};
}

json box_response()
{
    // Committed box_toolchanger API v1: four slots per physical box, followed
    // by the external holder. 'loaded' is unrelated to spool presence.
    return {{"result", {{"status", {{"box", {
        {"api_version", 1}, {"data_ready", true}, {"driver_ready", true},
        {"slots", {slot(0), slot(1, false), slot(2), slot(3), slot(4, true, true)}}
    }}}}}}};
}

class BoxServer
{
public:
    BoxServer(unsigned status, const std::string& body)
        : acceptor(io, {boost::asio::ip::address_v4::loopback(), 0}), socket(io)
    {
        port = acceptor.local_endpoint().port();
        response = "HTTP/1.1 " + std::to_string(status) + " Test\r\nContent-Length: " +
                   std::to_string(body.size()) + "\r\nConnection: close\r\n\r\n" + body;
        acceptor.async_accept(socket, [this, status](const boost::system::error_code& ec) {
            if (ec)
                return;
            if (status == 0) {
                socket.close();
                return;
            }
            boost::asio::async_read_until(socket, request_buffer, "\r\n\r\n",
                [this](const boost::system::error_code& ec, std::size_t) {
                    if (ec)
                        return;
                    request.assign(boost::asio::buffers_begin(request_buffer.data()),
                                   boost::asio::buffers_end(request_buffer.data()));
                    boost::asio::async_write(socket, boost::asio::buffer(response),
                        [this](const boost::system::error_code&, std::size_t) {
                            boost::system::error_code ignored;
                            socket.close(ignored);
                        });
                });
        });
        thread = std::thread([this] { io.run(); });
    }

    ~BoxServer() { finish(); }
    void finish()
    {
        io.stop();
        if (thread.joinable())
            thread.join();
    }
    std::string url() const { return "http://127.0.0.1:" + std::to_string(port); }
    std::string request;

private:
    boost::asio::io_context io;
    boost::asio::ip::tcp::acceptor acceptor;
    boost::asio::ip::tcp::socket socket;
    boost::asio::streambuf request_buffer;
    unsigned short port;
    std::string response;
    std::thread thread;
};

} // namespace

TEST_CASE("Box sync uses the configured Moonraker URL and API key", "[BoxFilamentSync]")
{
    BoxServer server(200, box_response().dump());
    const auto suffix = GENERATE(std::string("/moonraker"), std::string("/moonraker/"));
    std::vector<BoxFilamentSlot> slots;
    std::string error;
    const bool result = fetch_box_filament_status(server.url() + suffix, "test-api-key", slots, error);
    server.finish();
    INFO(error);
    REQUIRE(result);
    REQUIRE(slots.size() == 5);
    CHECK(server.request.find("GET /moonraker/printer/objects/query?box HTTP/1.1") != std::string::npos);
    CHECK(server.request.find("X-Api-Key: test-api-key") != std::string::npos);
}

TEST_CASE("Box sync declines HTTP errors so stock firmware can fall back", "[BoxFilamentSync]")
{
    const unsigned status = GENERATE(0u, 401u, 404u, 500u);
    BoxServer server(status, box_response().dump());
    std::vector<BoxFilamentSlot> slots(1);
    std::string error;
    CHECK_FALSE(fetch_box_filament_status(server.url(), "", slots, error));
    server.finish();
    CHECK(slots.empty());
    CHECK_FALSE(error.empty());
}

TEST_CASE("Box sync declines an incompatible successful HTTP response", "[BoxFilamentSync]")
{
    auto response = box_response();
    response["result"]["status"]["box"]["api_version"] = 2;
    BoxServer server(200, response.dump());
    std::vector<BoxFilamentSlot> slots(1);
    std::string error;
    CHECK_FALSE(fetch_box_filament_status(server.url(), "", slots, error));
    server.finish();
    CHECK(slots.empty());
    CHECK(error == "unsupported box API version");
}

TEST_CASE("Box sync keeps physical slot indices and spool metadata", "[BoxFilamentSync]")
{
    std::vector<BoxFilamentSlot> slots;
    std::string error;
    REQUIRE(parse_box_filament_status(box_response().dump(), slots, error));
    REQUIRE(slots.size() == 5);
    CHECK(error.empty());
    CHECK(slots[0].present);
    CHECK_FALSE(slots[1].present);
    CHECK(slots[2].index == 2);
    CHECK(slots[2].name == "My tuned PLA");
    CHECK(slots[2].brand == "Creality");
    CHECK(slots[2].material == "PLA");
    CHECK(slots[2].color == "#12AB34");
    CHECK(slots[4].external);
}

TEST_CASE("Box sync preserves gaps and sorts by physical tool number", "[BoxFilamentSync]")
{
    auto response = box_response();
    response["result"]["status"]["box"]["slots"] = {slot(8), slot(11), slot(10), slot(9), slot(12, true, true)};
    std::vector<BoxFilamentSlot> slots;
    std::string error;
    REQUIRE(parse_box_filament_status(response.dump(), slots, error));
    REQUIRE(slots.size() == 5);
    CHECK(slots.front().index == 8);
    CHECK(slots[1].index == 9);
    CHECK(slots.back().index == 12);
}

TEST_CASE("Box sync accepts an external holder without a CFS", "[BoxFilamentSync]")
{
    auto response = box_response();
    response["result"]["status"]["box"]["slots"] = {slot(0, true, true)};
    std::vector<BoxFilamentSlot> slots;
    std::string error;
    REQUIRE(parse_box_filament_status(response.dump(), slots, error));
    REQUIRE(slots.size() == 1);
    CHECK(slots[0].external);
    CHECK(slots[0].index == 0);
}

TEST_CASE("Box sync uses spool presence rather than the selected tool", "[BoxFilamentSync]")
{
    auto response = box_response();
    auto& data = response["result"]["status"]["box"]["slots"];
    data[1]["loaded"] = true;
    data[4]["material"] = "";
    data[4]["name"] = "";
    std::vector<BoxFilamentSlot> slots;
    std::string error;
    REQUIRE(parse_box_filament_status(response.dump(), slots, error));
    REQUIRE(slots.size() == 5);
    CHECK(slots[0].has_filament());
    CHECK_FALSE(slots[1].has_filament());
    CHECK_FALSE(slots[4].has_filament());
}

TEST_CASE("Box sync accepts four CFS units and the external holder", "[BoxFilamentSync]")
{
    auto response = box_response();
    auto& data = response["result"]["status"]["box"]["slots"];
    data = json::array();
    for (int i = 0; i < 16; ++i)
        data.push_back(slot(i));
    data.push_back(slot(16, true, true));
    std::vector<BoxFilamentSlot> slots;
    std::string error;
    REQUIRE(parse_box_filament_status(response.dump(), slots, error));
    REQUIRE(slots.size() == 17);
    CHECK(slots.back().index == 16);
}

TEST_CASE("Box sync declines unsupported versions and unready firmware", "[BoxFilamentSync]")
{
    auto response = box_response();
    auto& box = response["result"]["status"]["box"];
    const int scenario = GENERATE(0, 1, 2, 3, 4, 5, 6, 7);
    switch (scenario) {
    case 0: box.erase("api_version"); break;
    case 1: box["api_version"] = 0; break;
    case 2: box["api_version"] = 2; break;
    case 3: box["api_version"] = "1"; break;
    case 4: box["api_version"] = 1.0; break;
    case 5: box["data_ready"] = false; break;
    case 6: box["driver_ready"] = false; break;
    case 7: box["driver_ready"] = 1; break;
    }
    std::vector<BoxFilamentSlot> slots(1);
    std::string error;
    CHECK_FALSE(parse_box_filament_status(response.dump(), slots, error));
    CHECK(slots.empty());
    CHECK_FALSE(error.empty());
}

TEST_CASE("Box sync rejects malformed responses without retaining stale slots", "[BoxFilamentSync]")
{
    const auto response = GENERATE(std::string("not JSON"), std::string("null"), std::string("[]"),
                                  std::string(R"({"result":{"status":{}}})"),
                                  std::string(R"({"error":{"code":404,"message":"Unknown object"}})"));
    std::vector<BoxFilamentSlot> slots(1);
    std::string error;
    CHECK_FALSE(parse_box_filament_status(response, slots, error));
    CHECK(slots.empty());
    CHECK_FALSE(error.empty());
}

TEST_CASE("Box sync rejects ambiguous topology and malformed slot fields atomically", "[BoxFilamentSync]")
{
    auto response = box_response();
    auto& data = response["result"]["status"]["box"]["slots"];
    const int scenario = GENERATE(0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11);
    switch (scenario) {
    case 0: data[2]["index"] = 0; break;
    case 1: data[2]["index"] = -1; break;
    case 2: data[2]["index"] = 4294967296ULL; break;
    case 3: data[2]["index"] = 2.5; break;
    case 4: data[2]["present"] = "true"; break;
    case 5: data[2]["name"] = nullptr; break;
    case 6: data[2].erase("material"); break;
    case 7: data[4]["index"] = 8; break;
    case 8: data[2]["external"] = true; break;
    case 9: data.erase(2); break;
    case 10: data = json::array(); break;
    case 11: data = json::object(); break;
    }
    std::vector<BoxFilamentSlot> slots(1);
    std::string error;
    CHECK_FALSE(parse_box_filament_status(response.dump(), slots, error));
    CHECK(slots.empty());
    CHECK_FALSE(error.empty());
}
