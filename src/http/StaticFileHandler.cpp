#include "cofinance/http/StaticFileHandler.h"

#include <fstream>
#include <iterator>
#include <string>
#include <system_error>

namespace cofinance {
namespace http {
namespace {

constexpr std::uintmax_t kMaxStaticFileBytes = 8 * 1024 * 1024;

HttpResponse notFound() {
    return HttpResponse::text(404, "Not Found\n");
}

}  // namespace

StaticFileHandler::StaticFileHandler(std::string root) : root_(std::move(root)) {}

bool StaticFileHandler::init(std::string* err) {
    std::error_code ec;
    canonical_root_ = std::filesystem::weakly_canonical(root_, ec);
    if (ec || !std::filesystem::is_directory(canonical_root_, ec)) {
        if (err != nullptr) {
            *err = "静态文件目录不可用: " + root_.string();
        }
        return false;
    }
    return true;
}

HttpResponse StaticFileHandler::handle(const HttpRequest& request) const {
    if (request.method != "GET" && request.method != "HEAD") {
        HttpResponse response = HttpResponse::text(405, "Method Not Allowed\n");
        response.setHeader("Allow", "GET, HEAD");
        return response;
    }

    std::string relative;
    if (!safeRelativePath(request.path, &relative)) {
        return notFound();
    }

    std::error_code ec;
    std::filesystem::path candidate =
        std::filesystem::weakly_canonical(canonical_root_ / relative, ec);
    if (ec) {
        return notFound();
    }

    const std::filesystem::path normalized_relative =
        std::filesystem::relative(candidate, canonical_root_, ec);
    if (ec || normalized_relative.empty() || *normalized_relative.begin() == "..") {
        return notFound();
    }

    if (std::filesystem::is_directory(candidate, ec)) {
        candidate = std::filesystem::weakly_canonical(candidate / "index.html", ec);
        if (ec) {
            return notFound();
        }
    }
    if (!std::filesystem::is_regular_file(candidate, ec)) {
        return notFound();
    }
    const std::uintmax_t size = std::filesystem::file_size(candidate, ec);
    if (ec || size > kMaxStaticFileBytes) {
        return HttpResponse::text(413, "Static file too large\n");
    }

    std::ifstream input(candidate, std::ios::binary);
    if (!input) {
        return notFound();
    }
    std::string body((std::istreambuf_iterator<char>(input)), std::istreambuf_iterator<char>());
    if (!input.good() && !input.eof()) {
        return HttpResponse::text(500, "Failed to read static file\n");
    }

    HttpResponse response = HttpResponse::text(200, std::move(body));
    response.content_type = mimeType(candidate);
    return response;
}

std::string StaticFileHandler::mimeType(const std::filesystem::path& path) {
    std::string extension = path.extension().string();
    for (char& c : extension) {
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    }
    if (extension == ".html" || extension == ".htm") {
        return "text/html; charset=utf-8";
    }
    if (extension == ".css") {
        return "text/css; charset=utf-8";
    }
    if (extension == ".js") {
        return "application/javascript; charset=utf-8";
    }
    if (extension == ".json") {
        return "application/json; charset=utf-8";
    }
    if (extension == ".svg") {
        return "image/svg+xml";
    }
    if (extension == ".png") {
        return "image/png";
    }
    if (extension == ".jpg" || extension == ".jpeg") {
        return "image/jpeg";
    }
    if (extension == ".ico") {
        return "image/x-icon";
    }
    if (extension == ".woff2") {
        return "font/woff2";
    }
    return "application/octet-stream";
}

bool StaticFileHandler::safeRelativePath(const std::string& path, std::string* relative) {
    if (path.empty() || path.front() != '/') {
        return false;
    }
    std::string out;
    std::size_t begin = 0;
    while (begin < path.size()) {
        while (begin < path.size() && path[begin] == '/') {
            ++begin;
        }
        if (begin >= path.size()) {
            break;
        }
        const std::size_t slash = path.find('/', begin);
        const std::string segment =
            path.substr(begin, slash == std::string::npos ? std::string::npos : slash - begin);
        if (segment == ".." || segment.find('\\') != std::string::npos ||
            segment.find('\0') != std::string::npos) {
            return false;
        }
        if (!out.empty()) {
            out.push_back('/');
        }
        out += segment;
        if (slash == std::string::npos) {
            break;
        }
        begin = slash + 1;
    }
    if (out.empty() || path.back() == '/') {
        if (!out.empty()) {
            out.push_back('/');
        }
        out += "index.html";
    }
    if (relative != nullptr) {
        *relative = std::move(out);
    }
    return true;
}

}  // namespace http
}  // namespace cofinance
