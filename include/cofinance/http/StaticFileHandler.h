#pragma once

#include <filesystem>
#include <string>

#include "cofinance/http/HttpTypes.h"
#include "cofinance/util/Common.h"

namespace cofinance {
namespace http {

class StaticFileHandler : private NonCopyable {
public:
    explicit StaticFileHandler(std::string root);

    bool init(std::string* err);
    HttpResponse handle(const HttpRequest& request) const;

private:
    static std::string mimeType(const std::filesystem::path& path);
    static bool safeRelativePath(const std::string& path, std::string* relative);

    std::filesystem::path root_;
    std::filesystem::path canonical_root_;
};

}  // namespace http
}  // namespace cofinance
