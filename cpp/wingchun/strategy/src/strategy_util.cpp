//
// Created by qlu on 2019/1/29.
//

#include "strategy_util.h"
#include "portfolio/include/portfolio_manager.h"
#include "nn_publisher/nn_publisher.h"

#include "serialize.h"
#include "config.h"
#include "oms/include/def.h"

#include "util/include/env.h"
#include "util/include/filesystem_util.h"
#include "storage/account_list_storage.h"
#include "storage/snapshot_storage.h"

#include "Timer.h"
#include "JournalReader.h"

#include "fmt/format.h"
#include "Log.h"

namespace kungfu
{
#define DUMP_1D_SNAPSHOT(name, pnl) storage::SnapshotStorage(\
    STRATEGY_SNAPSHOT_DB_FILE(name), PORTFOLIO_ONE_DAY_SNAPSHOT_TABLE_NAME, true, false).insert(pnl)

#define DUMP_1M_SNAPSHOT(name, portfolio_manager) storage::SnapshotStorage(\
    STRATEGY_SNAPSHOT_DB_FILE(name), PORTFOLIO_ONE_MIN_SNAPSHOT_TABLE_NAME, false, false).insert(pnl)

    StrategyUtil::StrategyUtil(const std::string& name): name_(name), calendar_(new Calendar()), has_stock_account_(false), has_future_account_(false)
    {
        kungfu::yijinjing::KungfuLog::setup_log(name);
        kungfu::yijinjing::KungfuLog::set_log_level(spdlog::level::info);
        
        create_folder_if_not_exists(STRATEGY_FOLDER(name));

        std::string pub_url = STRATEGY_PUB_URL(name);
        publisher_ = std::shared_ptr<NNPublisher>(new NNPublisher(pub_url));

        storage::SnapshotStorage s1(STRATEGY_SNAPSHOT_DB_FILE(name), PORTFOLIO_ONE_DAY_SNAPSHOT_TABLE_NAME, true, false);
        storage::SnapshotStorage s2(STRATEGY_SNAPSHOT_DB_FILE(name), PORTFOLIO_ONE_MIN_SNAPSHOT_TABLE_NAME, false, false);

        calendar_->register_switch_day_callback(std::bind(&StrategyUtil::on_switch_day, this, std::placeholders::_1));

        int worker_id = UidWorkerStorage::get_instance(fmt::format(UID_WORKER_DB_FILE_FORMAT, get_base_dir()))->get_uid_worker_id(name);
        if (worker_id <= 0)
        {
            UidWorkerStorage::get_instance(fmt::format(UID_WORKER_DB_FILE_FORMAT, get_base_dir()))->add_uid_worker(name);
            worker_id = UidWorkerStorage::get_instance(fmt::format(UID_WORKER_DB_FILE_FORMAT, get_base_dir()))->get_uid_worker_id(name);
        }
        uid_generator_ = std::unique_ptr<UidGenerator>(new UidGenerator(worker_id, UID_EPOCH_SECONDS));

        writer_ = kungfu::yijinjing::JournalWriter::create(fmt::format(STRATEGY_JOURNAL_FOLDER_FORMAT, get_base_dir()), this->name_, this->name_);

        init_portfolio_manager();

        order_manager_ = oms::create_order_manager();

    }

    StrategyUtil::~StrategyUtil()
    {}

    bool StrategyUtil::add_md(const std::string &source_id)
    {
        storage::SourceListStorage(STRATEGY_MD_FEED_DB_FILE(name_)).add_source(source_id);
        auto rsp = gateway::add_market_feed(source_id, name_);
        if (rsp.state != GatewayState::Ready)
        {
            SPDLOG_WARN("market feed {} is not ready yet, current state: {}.", source_id, (int)rsp.state);
            return false;
        }
        else
        {
            return true;
        }
    }

    bool StrategyUtil::add_account(const std::string &source_id, const std::string &account_id,
                                   const double cash_limit)
    {

        StrategyUsedAccountInfo info = {};
        strcpy(info.client_id, this->name_.c_str());
        strcpy(info.source_id, source_id.c_str());
        strcpy(info.account_id, account_id.c_str());
        info.init_cash = cash_limit;

        storage::AccountListStorage(STRATEGY_ACCOUNT_LIST_DB_FILE(name_)).add_account(info);
        publisher_->publish_strategy_used_account(info);

        auto rsp = gateway::register_trade_account(source_id, account_id, this->name_);
        info.type = rsp.type;

        has_stock_account_ = has_stock_account_ || (info.type == AccountTypeStock || info.type == AccountTypeCredit);
        has_future_account_ = has_future_account_ || info.type == AccountTypeFuture;

        if (portfolio_manager_->get_account(account_id.c_str()) == nullptr)
        {
            AccountInfo acc = {};
            acc.update_time = yijinjing::getNanoTime();
            strcpy(acc.account_id, account_id.c_str());
            acc.type = rsp.type;
            acc.initial_equity = cash_limit;
            acc.static_equity = cash_limit;
            acc.dynamic_equity = cash_limit;
            acc.avail = cash_limit;
            portfolio_manager_->on_account(acc);
        }

        if (rsp.state != GatewayState::Ready)
        {
            SPDLOG_WARN("trade gateway {} is not ready yet, current state: {}.", TD_GATEWAY_NAME(source_id, account_id), (int)rsp.state);
            return false;
        }
        else
        {
            return true;
        }
    }

    void StrategyUtil::on_quote(const kungfu::Quote& quote)
    {
        SPDLOG_TRACE("instrument_id: {}, last_price: {}", quote.instrument_id, quote.last_price);
        quote_map_[get_symbol(quote.instrument_id, quote.exchange_id)] = quote;
        portfolio_manager_->on_quote(&quote);
    }

    void StrategyUtil::on_quote_py(uintptr_t quote)
    {
        on_quote(*(const kungfu::Quote*)quote);
    }

    void StrategyUtil::on_order(const kungfu::Order& order)
    {
        portfolio_manager_->on_order(&order);
        order_manager_->on_order(&order);
    }

    void StrategyUtil::on_order_py(uintptr_t order)
    {
        on_order(*(const kungfu::Order*)order);
    }

    void StrategyUtil::on_trade(const kungfu::Trade& trade)
    {
        portfolio_manager_->on_trade(&trade);
    }

    void StrategyUtil::on_trade_py(uintptr_t trade)
    {
        on_trade(*(const kungfu::Trade*)trade);
    }

    void StrategyUtil::on_algo_order_status(uint64_t order_id, const std::string& algo_type, const std::string& status_msg)
    {
        order_manager_->on_algo_order_status(order_id, algo_type, status_msg);
    }

    void StrategyUtil::register_switch_day_callback(std::function<void(const std::string &)> cb)
    {
        cbs_.emplace_back(cb);
    }

    bool StrategyUtil::register_algo_service()
    {
        //TODO
        return true;
    }

    void StrategyUtil::subscribe(const std::string &source, const std::vector<std::string> &instruments, const string &exchange_id, bool is_level2)
    {
        std::vector<Instrument> inst_vec;
        for (const auto& ins : instruments)
        {
            Instrument inst = {};
            strcpy(inst.instrument_id, ins.c_str());
            strcpy(inst.exchange_id, exchange_id.c_str());
            inst_vec.emplace_back(inst);
            subscribed_[source].insert(get_symbol(inst.instrument_id, inst.exchange_id));
        }
        kungfu::gateway::subscribe(source, inst_vec, is_level2, this->name_);
    }

    bool StrategyUtil::is_subscribed(const std::string &source, const std::string &instrument,
                                     const string &exchange_id) const
    {
        return subscribed_.find(source) != subscribed_.end() &&
            subscribed_.at(source).find(get_symbol(instrument, exchange_id)) != subscribed_.at(source).end();
    }

    uint64_t StrategyUtil::insert_limit_order(const std::string &instrument_id, const std::string &exchange_id,
                                              const std::string &account_id, double limit_price, int64_t volume,
                                              Side side, Offset offset)
    {
        OrderInput input = {};
        strcpy(input.instrument_id, instrument_id.c_str());
        strcpy(input.exchange_id, exchange_id.c_str());
        strcpy(input.account_id, account_id.c_str());
        input.limit_price = limit_price;
        input.frozen_price = limit_price;
        input.volume = volume;
        input.side = side;
        input.offset = offset;

        input.price_type = PriceTypeLimit;
        input.time_condition = TimeConditionGFD;
        input.volume_condition = VolumeConditionAny;

        return insert_order(input);
    }

    uint64_t StrategyUtil::insert_fak_order(const std::string &instrument_id, const std::string &exchange_id,
                                            const std::string &account_id, double limit_price, int64_t volume,
                                            Side side, Offset offset)
    {
        OrderInput input = {};

        strcpy(input.instrument_id, instrument_id.c_str());
        strcpy(input.exchange_id, exchange_id.c_str());
        strcpy(input.account_id, account_id.c_str());
        input.limit_price = limit_price;
        input.frozen_price = limit_price;
        input.volume = volume;
        input.side = side;
        input.offset = offset;

        input.price_type = PriceTypeLimit;
        input.time_condition = TimeConditionIOC;
        input.volume_condition = VolumeConditionAny;

        return insert_order(input);
    }

    uint64_t StrategyUtil::insert_fok_order(const std::string &instrument_id, const std::string &exchange_id,
                                            const std::string &account_id, double limit_price, int64_t volume,
                                            Side side, Offset offset)
    {
        OrderInput input = {};

        strcpy(input.instrument_id, instrument_id.c_str());
        strcpy(input.exchange_id, exchange_id.c_str());
        strcpy(input.account_id, account_id.c_str());
        input.limit_price = limit_price;
        input.frozen_price = limit_price;
        input.volume = volume;
        input.side = side;
        input.offset = offset;

        input.price_type = PriceTypeLimit;
        input.time_condition = TimeConditionIOC;
        input.volume_condition = VolumeConditionAll;

        return insert_order(input);
    }

    uint64_t StrategyUtil::insert_market_order(const std::string &instrument_id, const std::string &exchange_id,
                                               const std::string &account_id, int64_t volume, Side side, Offset offset)
    {
        OrderInput input = {};

        strcpy(input.instrument_id, instrument_id.c_str());
        strcpy(input.exchange_id, exchange_id.c_str());
        strcpy(input.account_id, account_id.c_str());
        auto iter = quote_map_.find(get_symbol(instrument_id, exchange_id));
        if (iter != quote_map_.end())
        {
            input.frozen_price = iter->second.last_price;
        }
        input.volume = volume;
        input.side = side;
        input.offset = offset;

        if (strcmp(input.exchange_id, EXCHANGE_SSE) == 0 || strcmp(input.exchange_id, EXCHANGE_SZE) == 0) //沪深市，最优五档转撤销
        {
            input.price_type = PriceTypeBest5;
            input.time_condition = TimeConditionIOC;
            input.volume_condition = VolumeConditionAny;
        }
        else
        {
            input.price_type = PriceTypeAny;
            input.time_condition = TimeConditionIOC;
            input.volume_condition = VolumeConditionAny;
        }
        return insert_order(input);
    }

    uint64_t StrategyUtil::insert_order(OrderInput& order_input)
    {
        if (nullptr == portfolio_manager_->get_account(order_input.account_id))
        {
            SPDLOG_ERROR("account {} has to be added in init before use", order_input.account_id);
            return 0;
        }

        uint64_t uid = next_id();
        order_input.order_id = uid;
        strcpy(order_input.client_id, this->name_.c_str());
        writer_->write_frame(&order_input, sizeof(OrderInput), -1, (int)MsgType::OrderInput, 1, -1);
        return uid;
    }

    uint64_t StrategyUtil::cancel_order(uint64_t order_id)
    {
        struct OrderAction action = {};

        uint64_t uid = next_id();
        action.order_action_id = uid;
        action.order_id = order_id;
        action.action_flag = OrderActionFlagCancel;

        writer_->write_frame(&action, sizeof(OrderAction), -1, (int)MsgType::OrderAction, true, -1);

        return uid;
    }

    uint64_t StrategyUtil::insert_algo_order(const std::string& algo_type, const std::string& order_input_msg)
    {
        uint64_t uid = next_id();
        AlgoOrderInput input = {};

        input.algo_type = algo_type;
        input.order_id = uid;
        input.client_id = std::string(this->name_.c_str());
        input.input = order_input_msg;
        std::string js = to_string(input);
        
        writer_->write_frame(js.c_str(), js.length() + 1, -1, (int)MsgType::AlgoOrderInput, true, -1 );
        return uid;
    }

    uint64_t StrategyUtil::modify_algo_order(uint64_t order_id, const std::string &cmd)
    {
        uint64_t uid = next_id();

        AlgoOrderAction action = {};
        action.order_action_id = uid;
        action.order_id = order_id;
        action.action = cmd;

        std::string js = to_string(action);

        writer_->write_frame(js.c_str(), js.length() + 1, -1, (int)MsgType::AlgoOrderAction, true, -1);

        return uid;
    }

    void StrategyUtil::set_log_level(int level)
    {
        kungfu::yijinjing::KungfuLog::set_log_level(level);
    }

    void StrategyUtil::log_info(const string &msg)
    {
        SPDLOG_INFO(msg);
    }

    void StrategyUtil::log_warn(const string &msg)
    {
        SPDLOG_WARN(msg);
    }

    void StrategyUtil::log_error(const string &msg)
    {
        SPDLOG_ERROR(msg);
    }

    const Quote* const StrategyUtil::get_last_md(const std::string& instrument_id, const std::string& exchange_id) const
    {
        auto iter = quote_map_.find(get_symbol(instrument_id, exchange_id));
        if (iter == quote_map_.end())
        {
            return nullptr;
        }
        else
        {
            return & iter->second;
        }
    }

    uintptr_t StrategyUtil::get_last_md_py(const std::string& instrument_id, const std::string& exchange_id) const
    {
        return  (uintptr_t) this->get_last_md(instrument_id, exchange_id);
    }

    void StrategyUtil::on_push_by_min()
    {
        long nano = yijinjing::getNanoTime();
        bool is_open = false;
        if (has_stock_account_)
        {
            is_open = is_open || calendar_->is_open(nano, EXCHANGE_SSE);
        }
        if (has_future_account_)
        {
            is_open = is_open || calendar_->is_open(nano, EXCHANGE_SHFE);
        }

        if (is_open)
        {
            auto pnl = *(portfolio_manager_->get_pnl());
            pnl.update_time = (int64_t)std::round((double)nano / 1000000000)* 1000000000;
            publisher_->publish_portfolio_info(pnl, kungfu::MsgType::PortfolioByMin);
            DUMP_1M_SNAPSHOT(name_, pnl);
        }
    }

    void StrategyUtil::on_push_by_day()
    {
        auto pnl = *(portfolio_manager_->get_pnl());
        pnl.update_time = (int64_t)std::round((double)yijinjing::getNanoTime() / 1000000000) * 1000000000;
        DUMP_1D_SNAPSHOT(name_, pnl);
    }

    double StrategyUtil::get_initial_equity() const
    {
        return portfolio_manager_->get_initial_equity();
    }

    double StrategyUtil::get_static_equity() const
    {
        return portfolio_manager_->get_static_equity();
    }
    double StrategyUtil::get_dynamic_equity() const
    {
        return portfolio_manager_->get_dynamic_equity();
    }

    double StrategyUtil::get_accumulated_pnl() const
    {
        return portfolio_manager_->get_accumulated_pnl();
    }

    double StrategyUtil::get_accumulated_pnl_ratio() const
    {
        return portfolio_manager_->get_accumulated_pnl_ratio();
    }

    double StrategyUtil::get_intraday_pnl() const
    {
        return portfolio_manager_->get_intraday_pnl();
    }

    double StrategyUtil::get_intraday_pnl_ratio() const
    {
        return portfolio_manager_->get_intraday_pnl_ratio();
    }

    int64_t StrategyUtil::get_long_tot(const string &instrument_id, const string &exchange_id) const
    {
        return portfolio_manager_->get_long_tot(instrument_id.c_str(), exchange_id.c_str());
    }

    int64_t StrategyUtil::get_long_tot_avail(const string &instrument_id, const string &exchange_id) const
    {
        return portfolio_manager_->get_long_tot_avail(instrument_id.c_str(), exchange_id.c_str());
    }

    int64_t StrategyUtil::get_long_tot_fro(const string &instrument_id, const string &exchange_id) const
    {
        return portfolio_manager_->get_long_tot_fro(instrument_id.c_str(), exchange_id.c_str());
    }

    int64_t StrategyUtil::get_long_yd(const string &instrument_id, const string &exchange_id) const
    {
        return portfolio_manager_->get_long_yd(instrument_id.c_str(), exchange_id.c_str());
    }

    int64_t StrategyUtil::get_long_yd_avail(const string &instrument_id, const string &exchange_id) const
    {
        return portfolio_manager_->get_long_yd_avail(instrument_id.c_str(), exchange_id.c_str());
    }

    int64_t StrategyUtil::get_long_yd_fro(const string &instrument_id, const string &exchange_id) const
    {
        return portfolio_manager_->get_long_yd_fro(instrument_id.c_str(), exchange_id.c_str());
    }

    double StrategyUtil::get_long_realized_pnl(const string &instrument_id, const string &exchange_id) const
    {
        return portfolio_manager_->get_long_realized_pnl(instrument_id.c_str(), exchange_id.c_str());
    }

    double StrategyUtil::get_long_unrealized_pnl(const string &instrument_id, const string &exchange_id) const
    {
        return portfolio_manager_->get_long_unrealized_pnl(instrument_id.c_str(), exchange_id.c_str());
    }

    double StrategyUtil::get_long_cost_price(const string &instrument_id, const string &exchange_id) const
    {
        return portfolio_manager_->get_long_cost_price(instrument_id.c_str(), exchange_id.c_str());
    }

    Position StrategyUtil::get_long_pos(const string &instrument_id, const string &exchange_id) const
    {
        return portfolio_manager_->get_long_pos(instrument_id.c_str(), exchange_id.c_str());
    }

    int64_t StrategyUtil::get_short_tot(const string &instrument_id, const string &exchange_id) const
    {
        return portfolio_manager_->get_short_tot(instrument_id.c_str(), exchange_id.c_str());
    }

    int64_t StrategyUtil::get_short_tot_avail(const string &instrument_id, const string &exchange_id) const
    {
        return portfolio_manager_->get_short_tot_avail(instrument_id.c_str(), exchange_id.c_str());
    }

    int64_t StrategyUtil::get_short_tot_fro(const string &instrument_id, const string &exchange_id) const
    {
        return portfolio_manager_->get_short_tot_fro(instrument_id.c_str(), exchange_id.c_str());
    }

    int64_t StrategyUtil::get_short_yd(const string &instrument_id, const string &exchange_id) const
    {
        return portfolio_manager_->get_short_yd(instrument_id.c_str(), exchange_id.c_str());
    }

    int64_t StrategyUtil::get_short_yd_avail(const string &instrument_id, const string &exchange_id) const
    {
        return portfolio_manager_->get_short_yd_avail(instrument_id.c_str(), exchange_id.c_str());
    }

    int64_t StrategyUtil::get_short_yd_fro(const string &instrument_id, const string &exchange_id) const
    {
        return portfolio_manager_->get_short_yd_fro(instrument_id.c_str(), exchange_id.c_str());
    }

    double StrategyUtil::get_short_realized_pnl(const string &instrument_id, const string &exchange_id) const
    {
        return portfolio_manager_->get_short_realized_pnl(instrument_id.c_str(), exchange_id.c_str());
    }

    double StrategyUtil::get_short_unrealized_pnl(const string &instrument_id, const string &exchange_id) const
    {
        return portfolio_manager_->get_short_unrealized_pnl(instrument_id.c_str(), exchange_id.c_str());
    }

    double StrategyUtil::get_short_cost_price(const string &instrument_id, const string &exchange_id) const
    {
        return portfolio_manager_->get_short_cost_price(instrument_id.c_str(), exchange_id.c_str());
    }

    Position StrategyUtil::get_short_pos(const string &instrument_id, const string &exchange_id) const
    {
        return portfolio_manager_->get_short_pos(instrument_id.c_str(), exchange_id.c_str());
    }

    uint64_t StrategyUtil::next_id()
    {
        long seconds = kungfu::yijinjing::getNanoTime() / kungfu::yijinjing::NANOSECONDS_PER_SECOND;
        return uid_generator_->next_id(seconds);
    }

    void StrategyUtil::on_switch_day(const std::string &trading_day)
    {
        if (nullptr != portfolio_manager_)
        {
            portfolio_manager_->switch_day(trading_day);
        }

        for (const auto& cb : cbs_)
        {
            cb(trading_day);
        }
    }

    std::vector<std::string> StrategyUtil::get_md_sources()
    {
        return storage::SourceListStorage(STRATEGY_MD_FEED_DB_FILE(name_)).get_sources();
    }

    std::vector<StrategyUsedAccountInfo> StrategyUtil::get_accounts()
    {
        return storage::AccountListStorage(STRATEGY_ACCOUNT_LIST_DB_FILE(name_)).get_accounts();
    }

    void StrategyUtil::init_portfolio_manager()
    {
        std::string asset_db_file = STRATEGY_ASSET_DB_FILE(this->name_);
        portfolio_manager_ = std::unique_ptr<PortfolioManager>(new PortfolioManager(asset_db_file.c_str()));

        portfolio_manager_->register_pos_callback(std::bind(&NNPublisher::publish_pos, publisher_.get(), std::placeholders::_1));
        portfolio_manager_->register_pnl_callback(std::bind(&NNPublisher::publish_portfolio_info, publisher_.get(), std::placeholders::_1,MsgType::Portfolio));

        int64_t last_update = portfolio_manager_->get_last_update();
        SPDLOG_TRACE("{} last update {}", name_, last_update);
        if (last_update > 0)
        {
            std::vector<std::string> folders;
            std::vector<std::string> names;
            for (const auto& source: get_md_sources())
            {
                folders.emplace_back(MD_JOURNAL_FOLDER(source));
                names.emplace_back(MD_JOURNAL_NAME(source));
            }
            for (const auto& account: get_accounts())
            {
                folders.emplace_back(TD_JOURNAL_FOLDER(account.source_id, account.account_id));
                names.emplace_back(TD_JOURNAL_NAME(account.source_id, account.account_id));
            }
            kungfu::yijinjing::JournalReaderPtr reader = kungfu::yijinjing::JournalReader::create(folders, names, last_update);
            kungfu::yijinjing::FramePtr frame = reader->getNextFrame();
            while (frame != nullptr)
            {
                int msg_type = frame->getMsgType();
                switch (msg_type)
                {
                    case (int) MsgType::Quote:
                    {
                        auto quote = (const Quote*) frame->getData();
                        if (quote->rcv_time > last_update)
                        {
                            portfolio_manager_->on_quote(quote);
                        }
                        break;
                    }
                    case (int) MsgType::Order:
                    {
                        auto* order = (const Order*) frame->getData();
                        if (strcmp(order->client_id, this->name_.c_str()) == 0 && order->rcv_time > last_update)
                        {
                            portfolio_manager_->on_order(order);
                        }
                        break;
                    }
                    case (int) MsgType::Trade:
                    {
                        auto* trade = (const Trade* ) frame->getData();
                        if (strcmp(trade->client_id, this->name_.c_str()) == 0 && trade->rcv_time > last_update)
                        {
                            portfolio_manager_->on_trade(trade);
                        }
                        break;
                    }
                    default:
                    {
                        break;
                    }
                }
                frame = reader->getNextFrame();
            }
            SPDLOG_INFO("forward portfolio manager from {}|{} to {}|{}", last_update, kungfu::yijinjing::parseNano(last_update, "%Y%m%d-%H:%M:%S"), portfolio_manager_->get_last_update(), kungfu::yijinjing::parseNano(portfolio_manager_->get_last_update(), "%Y%m%d-%H:%M:%S"));
        }
        portfolio_manager_->set_current_trading_day(calendar_->get_current_trading_day());
        SPDLOG_INFO("portfolio_manager inited and set trading_day to {}", portfolio_manager_->get_current_trading_day());
    }
}
