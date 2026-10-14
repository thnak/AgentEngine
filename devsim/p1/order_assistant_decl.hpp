// devsim/p1/order_assistant_decl.hpp -- the product's ONE declared tool and ONE declared agent.
//
// Split out of main.cpp only so the offline verifier (verify_tool_call.cpp) can prove that the SAME
// declaration the real-provider CLI ships is the one that works. Nothing here is a helper or a
// framework: it is the declaration itself, in the shape the docs show (web/marketing/src/data/
// apiContent.ts:711-720 minimalToolSnippet, :749-776 toolSchemaSnippet, :561-567 minimalAgentSnippet).

#pragma once

#include <string>
#include <string_view>
#include <vector>

#include "agentengine/core/agent.hpp"
#include "agentengine/core/effect_context.hpp"
#include "agentengine/core/json_schema.hpp"
#include "agentengine/core/tool.hpp"

namespace order_desk {

// ---------------------------------------------------------------------------------------------
// The one tool this product defines: look up an order by id, reply with a small JSON object.
// Five required members; declared_capabilities()/declared_approval()/declared_effect_class() come
// from Tool<Derived> itself (empty caps, never_require, at_most_once).
// ---------------------------------------------------------------------------------------------

struct LookupOrderArgs {
    agentengine::Described<std::string, "The order id to look up, e.g. '1042'."> order_id;
};
AE_JSON_SCHEMA(LookupOrderArgs, order_id)

struct OrderLine {
    std::string sku;
    int         quantity = 0;
};
AE_JSON_SCHEMA(OrderLine, sku, quantity)

struct LookupOrderReply {
    bool                   found = false;
    std::string            order_id;
    std::string            status;    // "packed" | "shipped" | "cancelled" | "not_found"
    std::string            customer;
    double                 total = 0.0;   // USD
    std::vector<OrderLine> lines;
};
AE_JSON_SCHEMA(LookupOrderReply, found, order_id, status, customer, total, lines)

// The order book behind the tool. A real deployment swaps this for a database call; what matters
// here is that the reply is a small, schema-typed JSON object and nothing else.
struct OrderRecord {
    std::string_view       id;
    std::string_view       status;
    std::string_view       customer;
    double                 total;
    std::vector<OrderLine> lines;
};

inline std::vector<OrderRecord> const& order_book() {
    static std::vector<OrderRecord> const book{
        {"1042", "shipped", "Nguyen Van A", 61.50, {{"AE-TEA-500", 2}, {"AE-MUG-01", 1}}},
        {"1043", "packed", "Tran Thi B", 24.00, {{"AE-BEAN-250", 3}}},
        {"1044", "cancelled", "Le Van C", 0.0, {}},
    };
    return book;
}

struct LookupOrderTool : agentengine::Tool<LookupOrderTool> {
    static constexpr std::string_view name = "lookup_order";
    static constexpr std::string_view description =
        "Look up one shop order by its order id. Returns the order's status, customer, total (USD) "
        "and line items. Use it instead of guessing anything about an order.";
    using Args  = LookupOrderArgs;
    using Reply = LookupOrderReply;

    static agentengine::result<Reply> invoke(Args args, agentengine::EffectContext&) {
        // `Described<T, "...">` converts implicitly to `T const&`, but template argument deduction
        // for `operator==` does not consider user-defined conversions -- so read the field out first.
        std::string const order_id = args.order_id;
        for (OrderRecord const& o : order_book()) {
            if (o.id == order_id) {
                return Reply{true, std::string{o.id}, std::string{o.status}, std::string{o.customer},
                             o.total, o.lines};
            }
        }
        return Reply{false, order_id, "not_found", {}, 0.0, {}};
    }
};

// ---------------------------------------------------------------------------------------------
// The agent: which backend, which tools, how many turns. Policies are types, not a config object.
// ---------------------------------------------------------------------------------------------

struct OrderAssistant
    : agentengine::Agent<OrderAssistant,
                         agentengine::ChatClientId<"anthropic:claude-sonnet-5">,
                         agentengine::Tools<LookupOrderTool>,
                         agentengine::MaxTurns<6>,
                         agentengine::TokenBudget<8'000>> {
    static constexpr std::string_view name = "order-assistant";
    static constexpr std::string_view instructions =
        "You are the order desk for a small business. When the customer asks about an order, call "
        "lookup_order and answer from what it returns. Never invent an order, a status or a total.";
};

}  // namespace order_desk
