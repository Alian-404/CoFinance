#pragma once

#include <string>

#include "cofinance/service/SettlementService.h"

namespace cofinance {
namespace ai {

struct AiPromptContext {
    std::string current_from;
    std::string current_to;
    std::string previous_from;
    std::string previous_to;
    service::GroupStats current;
    service::GroupStats previous;
};

// 只把聚合数据构造成 prompt。余额、备注、用户名、逐笔明细和聊天内容不会进入模型。
class PromptBuilder {
public:
    static std::string systemPrompt();
    static std::string askPrompt(const AiPromptContext& context, const std::string& question);
    static std::string suggestionsPrompt(const AiPromptContext& context);
};

}  // namespace ai
}  // namespace cofinance
