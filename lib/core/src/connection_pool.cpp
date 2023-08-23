#include "irods/connection_pool.hpp"

#include "irods/irods_exception.hpp"
#include "irods/irods_query.hpp"
#include "irods/rodsErrorTable.h"
#include "irods/thread_pool.hpp"

#include <algorithm>
#include <thread>
#include <tuple> // For std::ignore.

namespace irods
{
    //
    // Connection Proxy Implementation
    //

    connection_pool::connection_proxy::connection_proxy()
        : pool_{}
        , conn_{}
        , index_{uninitialized_index}
    {
    } // default constructor

    connection_pool::connection_proxy::connection_proxy(connection_proxy&& _other) noexcept
        : pool_{_other.pool_}
        , conn_{_other.conn_}
        , index_{_other.index_}
    {
        _other.pool_ = nullptr;
        _other.conn_ = nullptr;
        _other.index_ = uninitialized_index;
    } // move constructor

    connection_pool::connection_proxy& connection_pool::connection_proxy::operator=(connection_proxy&& _other) noexcept
    {
        pool_ = _other.pool_;
        conn_ = _other.conn_;
        index_ = _other.index_;

        _other.pool_ = nullptr;
        _other.conn_ = nullptr;
        _other.index_ = uninitialized_index;

        return *this;
    } // operator=

    connection_pool::connection_proxy::~connection_proxy()
    {
        // NOLINTNEXTLINE(readability-implicit-bool-conversion)
        if (pool_ && uninitialized_index != index_) {
            pool_->return_connection(index_);
        }
    } // destructor

    connection_pool::connection_proxy::operator bool() const noexcept
    {
        return nullptr != conn_;
    } // operator bool

    connection_pool::connection_proxy::operator RcComm&() const
    {
        if (!conn_) { // NOLINT(readability-implicit-bool-conversion)
            // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-array-to-pointer-decay)
            THROW(SYS_LIBRARY_ERROR, "Invalid connection object");
        }

        return *conn_;
    } // operator RcComm&

    connection_pool::connection_proxy::operator RcComm*() const noexcept
    {
        return conn_;
    } // operator RcComm*

    RcComm* connection_pool::connection_proxy::release()
    {
        pool_->release_connection(index_);
        auto* conn = conn_;
        conn_ = nullptr;
        return conn;
    } // release

    connection_pool::connection_proxy::connection_proxy(connection_pool& _pool, RcComm& _conn, int _index) noexcept
        : pool_{&_pool}
        , conn_{&_conn}
        , index_{_index}
    {
    } // constructor

    //
    // Connection Pool Implementation
    //

    connection_pool::connection_pool(int _size,
                                     const std::string& _host,
                                     const int _port,
                                     const std::string& _name,
                                     const std::string& _zone,
                                     [[maybe_unused]] const int _refresh_time)
        : connection_pool{_size, _host, _port, std::nullopt, {_name, _zone}, {}}
    {
    } // constructor

    connection_pool::connection_pool(int _size,
                                     const std::string& _host,
                                     const int _port,
                                     experimental::fully_qualified_username _username,
                                     const connection_pool_options& _options)
        : connection_pool{_size, _host, _port, std::nullopt, std::move(_username), {}, _options}
    {
    } // constructor

    connection_pool::connection_pool(int _size,
                                     const std::string& _host,
                                     const int _port,
                                     experimental::fully_qualified_username _username,
                                     std::function<void(RcComm&)> _auth_func,
                                     const connection_pool_options& _options)
        : connection_pool{_size, _host, _port, std::nullopt, std::move(_username), std::move(_auth_func), _options}
    {
    } // constructor

    connection_pool::connection_pool(int _size,
                                     std::string_view _host,
                                     const int _port,
                                     std::optional<experimental::fully_qualified_username> _proxy_username,
                                     experimental::fully_qualified_username _username,
                                     std::function<void(RcComm&)> _auth_func,
                                     const connection_pool_options& _options)
        : host_{_host}
        , port_{_port}
        , proxy_username_{std::move(_proxy_username)}
        , username_{std::move(_username)}
        , auth_func_{std::move(_auth_func)}
        , conn_ctxs_(_size)
        , options_{_options}
    {
        if (_size < 1) {
            // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-array-to-pointer-decay)
            THROW(SYS_INVALID_INPUT_PARAM, "Invalid connection pool size");
        }

        if (_options.number_of_retrievals_before_connection_refresh) {
            if (*_options.number_of_retrievals_before_connection_refresh <= 0) {
                // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-array-to-pointer-decay)
                THROW(SYS_INVALID_INPUT_PARAM,
                      "Value for option [number_of_retrievals_before_connection_refresh] must be greater than zero");
            }
        }

        if (_options.number_of_seconds_before_connection_refresh) {
            if (_options.number_of_seconds_before_connection_refresh->count() <= 0) {
                // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-array-to-pointer-decay)
                THROW(SYS_INVALID_INPUT_PARAM,
                      "Value for option [number_of_seconds_before_connection_refresh] must be greater than zero");
            }
        }

        // Always initialize the first connection to guarantee that the
        // network plugin is loaded. This guarantees that asynchronous calls
        // to rcConnect do not cause a segfault.
        create_connection(
            0,
            // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-array-to-pointer-decay)
            [] { THROW(USER_SOCK_CONNECT_ERR, "Connect error"); },
            // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-array-to-pointer-decay)
            [] { THROW(AUTHENTICATION_ERROR, "Client login error"); });

        if (_options.refresh_connections_when_resource_changes_detected) {
            std::uint64_t latest_mtime = 0;

            // Capture the latest time any resource was modified.
            // This will be used to track when a connection should be refreshed.
            // This helps with long-running agents.
            for (auto&& row : irods::query{conn_ctxs_[0].conn.get(), "select max(RESC_MODIFY_TIME)"}) {
                try {
                    latest_mtime = std::stoull(row[0]);
                }
                catch (...) {
                    // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-array-to-pointer-decay)
                    THROW(SYS_LIBRARY_ERROR, "Failed to convert RESC_MODIFY_TIME to an integer");
                }
            }

            // Attach the mtime to each connection context.
            std::for_each(std::begin(conn_ctxs_), std::end(conn_ctxs_), [latest_mtime](auto&& _ctx) {
                _ctx.latest_resc_mtime = latest_mtime;
            });
        }

        // If the size of the pool is one, then return immediately.
        if (_size == 1) {
            return;
        }

        // Initialize the rest of the connection pool asynchronously.

        irods::thread_pool thread_pool{std::min<int>(_size, static_cast<int>(std::thread::hardware_concurrency()))};

        std::atomic<bool> connect_error{};
        std::atomic<bool> login_error{};

        const auto on_connect_error = [&connect_error] { connect_error.store(true); };
        const auto on_login_error = [&login_error] { login_error.store(true); };

        for (int i = 1; i < _size; ++i) {
            irods::thread_pool::post(
                thread_pool, [this, i, &connect_error, &login_error, &on_connect_error, &on_login_error] {
                    if (connect_error.load() || login_error.load()) {
                        return;
                    }

                    create_connection(i, on_connect_error, on_login_error);
                });
        }

        thread_pool.join();

        if (connect_error.load()) {
            // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-array-to-pointer-decay)
            THROW(USER_SOCK_CONNECT_ERR, "Connect error");
        }

        if (login_error.load()) {
            // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-array-to-pointer-decay)
            THROW(AUTHENTICATION_ERROR, "Client login error");
        }
    } // constructor

    void connection_pool::create_connection(int _index,
                                            // NOLINTNEXTLINE(bugprone-easily-swappable-parameters)
                                            const std::function<void()>& _on_connect_error,
                                            const std::function<void()>& _on_login_error)
    {
        auto& ctx = conn_ctxs_[_index];

        if (options_.number_of_seconds_before_connection_refresh) {
            ctx.creation_time = std::chrono::steady_clock::now();
        }

        if (options_.number_of_retrievals_before_connection_refresh) {
            ctx.retrieval_count = 0;
        }

        if (proxy_username_.has_value()) {
            ctx.conn.reset(_rcConnect(host_.c_str(),
                                      port_,
                                      proxy_username_->name().c_str(),
                                      proxy_username_->zone().c_str(),
                                      username_.name().c_str(),
                                      username_.zone().c_str(),
                                      &ctx.error,
                                      1,
                                      NO_RECONN));
        }
        else {
            ctx.conn.reset(rcConnect(
                host_.c_str(), port_, username_.name().c_str(), username_.zone().c_str(), NO_RECONN, &ctx.error));
        }

        if (!ctx.conn) {
            _on_connect_error();
            return;
        }

        if (auth_func_) {
            auth_func_(*ctx.conn);
            return;
        }

        if (clientLogin(ctx.conn.get()) != 0) {
            _on_login_error();
        }
    } // create_connection

    bool connection_pool::verify_connection(int _index)
    {
        auto& ctx = conn_ctxs_[_index];

        if (!ctx.conn) {
            return false;
        }

        try {
            // Check if the number of retrievals via get_connection() has been reached.
            if (options_.number_of_retrievals_before_connection_refresh) {
                if (ctx.retrieval_count >= *options_.number_of_retrievals_before_connection_refresh) {
                    return false;
                }
            }

            // Check age of connection.
            if (options_.number_of_seconds_before_connection_refresh) {
                const auto elapsed = std::chrono::steady_clock::now() - ctx.creation_time;

                if (elapsed >= *options_.number_of_seconds_before_connection_refresh) {
                    return false;
                }
            }

            // Check if any resources have been modified.
            if (options_.refresh_connections_when_resource_changes_detected) {
                for (auto&& row : irods::query{ctx.conn.get(), "select max(RESC_MODIFY_TIME)"}) {
                    try {
                        if (const auto mtime = std::stoull(row[0]); mtime > ctx.latest_resc_mtime) {
                            ctx.latest_resc_mtime = mtime;
                            return false;
                        }
                    }
                    catch (...) {
                        // In theory, we should never reach this block. However, if we do, just
                        // go ahead and refresh the connection. At the very least, the agent will
                        // reflect the latest state in the catalog for a small penalty of TCP socket
                        // reconstruction.
                        return false;
                    }
                }
            }

            // Check if the connection is still valid.
            // This query will always succeed unless there's an issue in which case an exception will
            // be thrown.
            query{ctx.conn.get(), "select ZONE_NAME where ZONE_TYPE = 'local'"}; // NOLINT(bugprone-unused-raii)
        }
        catch (const std::exception&) {
            return false;
        }

        return true;
    } // verify_connection

    RcComm* connection_pool::refresh_connection(int _index)
    {
        auto& ctx = conn_ctxs_[_index];
        ctx.error = {};

        if (ctx.refresh) {
            ctx.refresh = false;
        }

        if (!verify_connection(_index)) {
            create_connection(
                _index,
                // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-array-to-pointer-decay)
                [] { THROW(USER_SOCK_CONNECT_ERR, "Connect error"); },
                // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-array-to-pointer-decay)
                [] { THROW(AUTHENTICATION_ERROR, "Client login error"); });
        }

        return ctx.conn.get();
    } // refresh_connection

    connection_pool::connection_proxy connection_pool::get_connection()
    {
        for (int i = 0;; i = (i + 1) % static_cast<int>(conn_ctxs_.size())) {
            std::unique_lock<std::mutex> lock{conn_ctxs_[i].mutex, std::defer_lock};

            if (lock.try_lock()) {
                if (!conn_ctxs_[i].in_use.load()) {
                    conn_ctxs_[i].in_use.store(true);
                    connection_proxy proxy{*this, *refresh_connection(i), i};

                    if (options_.number_of_retrievals_before_connection_refresh) {
                        ++conn_ctxs_[i].retrieval_count;
                    }

                    return proxy;
                }
            }
        }
    } // get_connection

    void connection_pool::return_connection(int _index)
    {
        conn_ctxs_[_index].in_use.store(false);
    } // return_connection

    void connection_pool::release_connection(int _index)
    {
        conn_ctxs_[_index].refresh = true;
        std::ignore = conn_ctxs_[_index].conn.release();
    } // release_connection

    std::shared_ptr<connection_pool> make_connection_pool(int _size)
    {
        rodsEnv env{};
        _getRodsEnv(env);
        return std::make_shared<irods::connection_pool>(
            _size, env.rodsHost, env.rodsPort, env.rodsUserName, env.rodsZone, env.irodsConnectionPoolRefreshTime);
    } // make_connection_pool
} // namespace irods
