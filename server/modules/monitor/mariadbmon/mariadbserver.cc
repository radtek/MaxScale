/*
 * Copyright (c) 2018 MariaDB Corporation Ab
 *
 * Use of this software is governed by the Business Source License included
 * in the LICENSE.TXT file and at www.mariadb.com/bsl11.
 *
 * Change Date: 2022-01-01
 *
 * On the date above, in accordance with the Business Source License, use
 * of this software will be governed by version 2 or later of the General
 * Public License.
 */

#include "mariadbserver.hh"

#include <fstream>
#include <inttypes.h>
#include <iomanip>
#include <thread>
#include <maxscale/mysql_utils.h>
#include <maxscale/utils.hh>
#include <set>

using std::string;
using maxscale::string_printf;
using maxbase::Duration;
using maxbase::StopWatch;
using Guard = std::lock_guard<std::mutex>;

MariaDBServer::MariaDBServer(MXS_MONITORED_SERVER* monitored_server, int config_index,
                             bool assume_unique_hostnames, bool query_events)
    : m_server_base(monitored_server)
    , m_config_index(config_index)
    , m_assume_unique_hostnames(assume_unique_hostnames)
    , m_query_events(query_events)
{
    mxb_assert(monitored_server);
}

NodeData::NodeData()
    : index(INDEX_NOT_VISITED)
    , lowest_index(INDEX_NOT_VISITED)
    , in_stack(false)
    , cycle(CYCLE_NONE)
    , reach(REACH_UNKNOWN)
{
}

void NodeData::reset_results()
{
    cycle = CYCLE_NONE;
    reach = REACH_UNKNOWN;
    parents.clear();
    children.clear();
    external_masters.clear();
}

void NodeData::reset_indexes()
{
    index = INDEX_NOT_VISITED;
    lowest_index = INDEX_NOT_VISITED;
    in_stack = false;
}

uint64_t MariaDBServer::relay_log_events(const SlaveStatus& slave_conn)
{
    /* The events_ahead-call below ignores domains where current_pos is ahead of io_pos. This situation is
     * rare but is possible (I guess?) if the server is replicating a domain from multiple masters
     * and decides to process events from one relay log before getting new events to the other. In
     * any case, such events are obsolete and the server can be considered to have processed such logs. */
    return slave_conn.gtid_io_pos.events_ahead(m_gtid_current_pos, GtidList::MISSING_DOMAIN_IGNORE);
}

std::unique_ptr<QueryResult> MariaDBServer::execute_query(const string& query, std::string* errmsg_out,
                                                          unsigned int* errno_out)
{
    auto conn = m_server_base->con;
    std::unique_ptr<QueryResult> rval;
    MYSQL_RES* result = NULL;
    if (mxs_mysql_query(conn, query.c_str()) == 0 && (result = mysql_store_result(conn)) != NULL)
    {
        rval = std::unique_ptr<QueryResult>(new QueryResult(result));
    }
    else
    {
        if (errmsg_out)
        {
            *errmsg_out = string_printf("Query '%s' failed: '%s'.", query.c_str(), mysql_error(conn));
        }
        if (errno_out)
        {
            *errno_out = mysql_errno(conn);
        }
    }
    return rval;
}

/**
 * Execute a query which does not return data. If the query returns data, an error is returned.
 *
 * @param cmd The query
 * @param mode Retry a failed query using the global query retry settings or not
 * @param errmsg_out Error output.
 * @return True on success, false on error or if query returned data
 */
bool MariaDBServer::execute_cmd_ex(const string& cmd, QueryRetryMode mode,
                                   std::string* errmsg_out, unsigned int* errno_out)
{
    auto conn = m_server_base->con;
    bool query_success = false;
    if (mode == QueryRetryMode::ENABLED)
    {
        query_success = (mxs_mysql_query(conn, cmd.c_str()) == 0);
    }
    else
    {
        query_success = (mxs_mysql_query_ex(conn, cmd.c_str(), 0, 0) == 0);
    }

    bool rval = false;
    if (query_success)
    {
        MYSQL_RES* result = mysql_store_result(conn);
        if (result == NULL)
        {
            rval = true;
        }
        else if (errmsg_out)
        {
            int cols = mysql_num_fields(result);
            int rows = mysql_num_rows(result);
            *errmsg_out = string_printf("Query '%s' on '%s' returned %d columns and %d rows of data when "
                                        "none was expected.", cmd.c_str(), name(), cols, rows);
        }
    }
    else
    {
        if (errmsg_out)
        {
            *errmsg_out = string_printf("Query '%s' failed on '%s': '%s' (%i).",
                                        cmd.c_str(), name(), mysql_error(conn), mysql_errno(conn));
        }
        if (errno_out)
        {
            *errno_out = mysql_errno(conn);
        }
    }
    return rval;
}

bool MariaDBServer::execute_cmd(const std::string& cmd, std::string* errmsg_out)
{
    return execute_cmd_ex(cmd, QueryRetryMode::ENABLED, errmsg_out);
}

bool MariaDBServer::execute_cmd_no_retry(const std::string& cmd,
                                         std::string* errmsg_out, unsigned int* errno_out)
{
    return execute_cmd_ex(cmd, QueryRetryMode::DISABLED, errmsg_out, errno_out);
}

/**
 * Execute a query which does not return data. If the query fails because of a network error
 * (e.g. Connector-C timeout), automatically retry the query until time is up. Uses max_statement_time
 * when available to ensure no lingering timed out commands are left on the server.
 *
 * @param cmd The query to execute. Should be a query with a predictable effect even when retried or
 * ran several times.
 * @param time_limit How long to retry. This does not overwrite the connector-c timeouts which are always
 * respected.
 * @param errmsg_out Error output
 * @return True, if successful.
 */
bool MariaDBServer::execute_cmd_time_limit(const std::string& cmd, maxbase::Duration time_limit,
                                           std::string* errmsg_out)
{
    StopWatch timer;
    string max_stmt_time;
    int connector_timeout = -1;
    if (m_capabilities.max_statement_time)
    {
        MXB_AT_DEBUG(int rv = ) mysql_get_optionv(m_server_base->con, MYSQL_OPT_READ_TIMEOUT,
                                                  &connector_timeout);
        mxb_assert(rv == 0);
        if (connector_timeout > 0)
        {
            max_stmt_time = string_printf("SET STATEMENT max_statement_time=%i FOR ", connector_timeout);
        }
    }

    const string command = max_stmt_time + cmd;
    // If a query lasts less than 1s, sleep so that at most 1 query/s is sent.
    // This prevents busy-looping when faced with some network errors.
    const Duration min_query_time(1.0);

    // Even if time is up, try at least once.
    bool cmd_success = false;
    bool keep_trying = true;
    while (!cmd_success && keep_trying)
    {
        StopWatch query_timer;
        string error_msg;
        unsigned int errornum = 0;
        cmd_success = execute_cmd_no_retry(command, &error_msg, &errornum);
        auto query_time = query_timer.lap();

        // Check if there is time to retry.
        Duration time_remaining = time_limit - timer.split();
        bool non_fatal_connector_err = mxs_mysql_is_net_error(errornum);
        keep_trying = (time_remaining.secs() > 0)
            // Either a connector-c timeout or query was interrupted by max_statement_time.
            && (non_fatal_connector_err || (!max_stmt_time.empty() && errornum == ER_STATEMENT_TIMEOUT));

        if (!cmd_success)
        {
            if (keep_trying)
            {
                string retrying = string_printf("Retrying with %.1f seconds left.", time_remaining.secs());
                if (non_fatal_connector_err)
                {
                    MXS_WARNING("%s %s", error_msg.c_str(), retrying.c_str());
                }
                else
                {
                    // Timed out because of max_statement_time.
                    MXS_WARNING("Query '%s' timed out on '%s'. %s",
                                command.c_str(), name(), retrying.c_str());
                }

                if (query_time < min_query_time)
                {
                    Duration query_sleep = min_query_time - query_time;
                    Duration this_sleep = MXS_MIN(time_remaining, query_sleep);
                    std::this_thread::sleep_for(this_sleep);
                }
            }
            else if (errmsg_out)
            {
                *errmsg_out = error_msg; // The error string already has all required info.
            }
        }
    }
    return cmd_success;
}

bool MariaDBServer::do_show_slave_status(string* errmsg_out)
{
    unsigned int columns = 0;
    string query;
    bool all_slaves_status = false;
    if (m_capabilities.gtid || m_srv_type == server_type::BINLOG_ROUTER)
    {
        // Versions with gtid also support the extended slave status query.
        columns = 42;
        all_slaves_status = true;
        query = "SHOW ALL SLAVES STATUS;";
    }
    else if (m_capabilities.basic_support)
    {
        columns = 40;
        query = "SHOW SLAVE STATUS;";
    }
    else
    {
        mxb_assert(!true);      // This method should not be called for versions < 5.5
        return false;
    }

    auto result = execute_query(query, errmsg_out);
    if (result.get() == NULL)
    {
        return false;
    }
    else if (result->get_col_count() < columns)
    {
        MXS_ERROR("'%s' returned less than the expected amount of columns. Expected %u columns, "
                  "got %" PRId64 ".",
                  query.c_str(),
                  columns,
                  result->get_col_count());
        return false;
    }

    // Fields common to all server versions
    auto i_master_host = result->get_col_index("Master_Host");
    auto i_master_port = result->get_col_index("Master_Port");
    auto i_slave_io_running = result->get_col_index("Slave_IO_Running");
    auto i_slave_sql_running = result->get_col_index("Slave_SQL_Running");
    auto i_master_server_id = result->get_col_index("Master_Server_Id");
    auto i_last_io_errno = result->get_col_index("Last_IO_Errno");
    auto i_last_io_error = result->get_col_index("Last_IO_Error");
    auto i_last_sql_error = result->get_col_index("Last_SQL_Error");
    auto i_seconds_behind_master = result->get_col_index("Seconds_Behind_Master");

    const char INVALID_DATA[] = "'%s' returned invalid data.";
    if (i_master_host < 0 || i_master_port < 0 || i_slave_io_running < 0 || i_slave_sql_running < 0
        || i_master_server_id < 0 || i_last_io_errno < 0 || i_last_io_error < 0 || i_last_sql_error < 0
        || i_seconds_behind_master < 0)
    {
        MXS_ERROR(INVALID_DATA, query.c_str());
        return false;
    }

    int64_t i_connection_name = -1, i_slave_rec_hbs = -1, i_slave_hb_period = -1;
    int64_t i_using_gtid = -1, i_gtid_io_pos = -1;
    if (all_slaves_status)
    {
        i_connection_name = result->get_col_index("Connection_name");
        i_slave_rec_hbs = result->get_col_index("Slave_received_heartbeats");
        i_slave_hb_period = result->get_col_index("Slave_heartbeat_period");
        i_using_gtid = result->get_col_index("Using_Gtid");
        i_gtid_io_pos = result->get_col_index("Gtid_IO_Pos");
        if (i_connection_name < 0 || i_slave_rec_hbs < 0 || i_slave_hb_period < 0
            || i_using_gtid < 0 || i_gtid_io_pos < 0)
        {
            MXS_ERROR(INVALID_DATA, query.c_str());
            return false;
        }
    }

    SlaveStatusArray slave_status_new;
    while (result->next_row())
    {
        SlaveStatus new_row;
        new_row.owning_server = name();
        new_row.master_host = result->get_string(i_master_host);
        new_row.master_port = result->get_uint(i_master_port);
        string last_io_error = result->get_string(i_last_io_error);
        string last_sql_error = result->get_string(i_last_sql_error);
        new_row.last_error = !last_io_error.empty() ? last_io_error : last_sql_error;

        new_row.slave_io_running =
            SlaveStatus::slave_io_from_string(result->get_string(i_slave_io_running));
        new_row.slave_sql_running = (result->get_string(i_slave_sql_running) == "Yes");
        new_row.master_server_id = result->get_uint(i_master_server_id);

        auto rlag = result->get_uint(i_seconds_behind_master);
        // If slave connection is stopped, the value given by the backend is null -> -1.
        new_row.seconds_behind_master = (rlag < 0) ? MXS_RLAG_UNDEFINED :
            (rlag > INT_MAX) ? INT_MAX : rlag;

        if (all_slaves_status)
        {
            new_row.name = result->get_string(i_connection_name);
            new_row.received_heartbeats = result->get_uint(i_slave_rec_hbs);

            string using_gtid = result->get_string(i_using_gtid);
            string gtid_io_pos = result->get_string(i_gtid_io_pos);
            if (!gtid_io_pos.empty() && (using_gtid == "Current_Pos" || using_gtid == "Slave_Pos"))
            {
                new_row.gtid_io_pos = GtidList::from_string(gtid_io_pos);
            }
        }

        // Before adding this row to the SlaveStatus array, compare the row to the one from the previous
        // monitor tick and fill in the last pieces of data.
        auto old_row = sstatus_find_previous_row(new_row, slave_status_new.size());
        if (old_row)
        {
            // When the new row was created, 'last_data_time' was set to the current time. If it seems
            // like the slave is not receiving data from the master, set the time to the one
            // in the previous monitor tick.
            if (new_row.received_heartbeats == old_row->received_heartbeats
                && new_row.gtid_io_pos == old_row->gtid_io_pos)
            {
                new_row.last_data_time = old_row->last_data_time;
            }
        }

        // Finally, set the connection status.
        if (new_row.slave_io_running == SlaveStatus::SLAVE_IO_YES)
        {
            mxb_assert(new_row.master_server_id > 0);
            new_row.seen_connected = true;
        }
        else if (new_row.slave_io_running == SlaveStatus::SLAVE_IO_CONNECTING && old_row)
        {
            // Old connection data found. Even in this case the server id:s could be wrong if the
            // slave connection was cleared and remade between monitor loops.
            if (new_row.master_server_id == old_row->master_server_id && old_row->seen_connected)
            {
                new_row.seen_connected = true;
            }
        }

        // Row complete, add it to the array.
        slave_status_new.push_back(new_row);
    }

    // Compare the previous array to the new one.
    if (!sstatus_array_topology_equal(slave_status_new))
    {
        m_topology_changed = true;
    }

    // Always write to m_slave_status. Even if the new status is equal by topology,
    // gtid:s etc may have changed.
    Guard guard(m_arraylock);
    m_slave_status = std::move(slave_status_new);
    return true;
}

bool MariaDBServer::update_gtids(string* errmsg_out)
{
    static const string query = "SELECT @@gtid_current_pos, @@gtid_binlog_pos;";
    const int i_current_pos = 0;
    const int i_binlog_pos = 1;

    bool rval = false;
    auto result = execute_query(query, errmsg_out);
    if (result.get() != NULL)
    {
        Guard guard(m_arraylock);

        rval = true;
        if (result->next_row())
        {
            // Query returned at least some data.
            auto current_str = result->get_string(i_current_pos);
            auto binlog_str = result->get_string(i_binlog_pos);
            if (current_str.empty())
            {
                m_gtid_current_pos = GtidList();
            }
            else
            {
                m_gtid_current_pos = GtidList::from_string(current_str);
            }

            if (binlog_str.empty())
            {
                m_gtid_binlog_pos = GtidList();
            }
            else
            {
                m_gtid_binlog_pos = GtidList::from_string(binlog_str);
            }
        }
        else
        {
            // Query succeeded but returned 0 rows. This means that the server has no gtid:s.
            m_gtid_current_pos = GtidList();
            m_gtid_binlog_pos = GtidList();
        }
    } // If query failed, do not update gtid:s.
    return rval;
}

bool MariaDBServer::update_replication_settings(std::string* errmsg_out)
{
    const string query = "SELECT @@gtid_strict_mode, @@log_bin, @@log_slave_updates;";
    bool rval = false;

    auto result = execute_query(query, errmsg_out);
    if (result.get() != NULL && result->next_row())
    {
        rval = true;
        m_rpl_settings.gtid_strict_mode = result->get_bool(0);
        m_rpl_settings.log_bin = result->get_bool(1);
        m_rpl_settings.log_slave_updates = result->get_bool(2);
    }
    return rval;
}

bool MariaDBServer::read_server_variables(string* errmsg_out)
{
    const string query_no_gtid = "SELECT @@global.server_id, @@read_only;";
    const string query_with_gtid = "SELECT @@global.server_id, @@read_only, @@global.gtid_domain_id;";
    const bool use_gtid = m_capabilities.gtid;
    const string& query = use_gtid ? query_with_gtid : query_no_gtid;

    int i_id = 0;
    int i_ro = 1;
    int i_domain = 2;
    bool rval = false;
    auto result = execute_query(query, errmsg_out);
    if (result.get() != NULL && result->next_row())
    {
        rval = true;
        int64_t server_id_parsed = result->get_uint(i_id);
        if (server_id_parsed < 0)   // This is very unlikely, requiring an error in server or connector.
        {
            server_id_parsed = SERVER_ID_UNKNOWN;
            rval = false;
        }
        if (server_id_parsed != m_server_id)
        {
            m_server_id = server_id_parsed;
            m_topology_changed = true;
        }
        m_server_base->server->node_id = server_id_parsed;

        bool read_only_parsed = result->get_bool(i_ro);
        if (read_only_parsed != m_read_only)
        {
            m_read_only = read_only_parsed;
            m_topology_changed = true;
        }

        if (use_gtid)
        {
            int64_t domain_id_parsed = result->get_uint(i_domain);
            if (domain_id_parsed < 0)   // Same here.
            {
                domain_id_parsed = GTID_DOMAIN_UNKNOWN;
                rval = false;
            }
            m_gtid_domain_id = domain_id_parsed;
        }
        else
        {
            m_gtid_domain_id = GTID_DOMAIN_UNKNOWN;
        }
    }
    return rval;
}

void MariaDBServer::warn_replication_settings() const
{
    const char* servername = name();
    if (m_rpl_settings.gtid_strict_mode == false)
    {
        const char NO_STRICT[] =
            "Slave '%s' has gtid_strict_mode disabled. Enabling this setting is recommended. "
            "For more information, see https://mariadb.com/kb/en/library/gtid/#gtid_strict_mode";
        MXS_WARNING(NO_STRICT, servername);
    }
    if (m_rpl_settings.log_slave_updates == false)
    {
        const char NO_SLAVE_UPDATES[] =
            "Slave '%s' has log_slave_updates disabled. It is a valid candidate but replication "
            "will break for lagging slaves if '%s' is promoted.";
        MXS_WARNING(NO_SLAVE_UPDATES, servername, servername);
    }
}

bool MariaDBServer::catchup_to_master(GeneralOpData& op, const GtidList& target)
{
    /* Prefer to use gtid_binlog_pos, as that is more reliable. But if log_slave_updates is not on,
     * use gtid_current_pos. */
    const bool use_binlog_pos = m_rpl_settings.log_bin && m_rpl_settings.log_slave_updates;
    bool time_is_up = false;    // Check at least once.
    bool gtid_reached = false;
    bool error = false;
    json_t** error_out = op.error_out;

    Duration sleep_time(0.2);   // How long to sleep before next iteration. Incremented slowly.
    StopWatch timer;

    while (!time_is_up && !gtid_reached && !error)
    {
        string error_msg;
        if (update_gtids(&error_msg))
        {
            const GtidList& compare_to = use_binlog_pos ? m_gtid_binlog_pos : m_gtid_current_pos;
            if (target.events_ahead(compare_to, GtidList::MISSING_DOMAIN_IGNORE) == 0)
            {
                gtid_reached = true;
            }
            else
            {
                // Query was successful but target gtid not yet reached. Check how much time left.
                op.time_remaining -= timer.lap();
                if (op.time_remaining.secs() > 0)
                {
                    // Sleep for a moment, then try again.
                    Duration this_sleep = MXS_MIN(sleep_time, op.time_remaining);
                    std::this_thread::sleep_for(this_sleep);
                    sleep_time += Duration(0.1);    // Sleep a bit more next iteration.
                }
                else
                {
                    time_is_up = true;
                }
            }
        }
        else
        {
            error = true;
            PRINT_MXS_JSON_ERROR(error_out, "Failed to update gtid on '%s' while waiting for catchup: %s",
                                 name(), error_msg.c_str());
        }
    }

    if (!error && !gtid_reached)
    {
        PRINT_MXS_JSON_ERROR(error_out, "Slave catchup timed out on slave '%s'.", name());
    }
    return gtid_reached;
}

bool MariaDBServer::binlog_on() const
{
    return m_rpl_settings.log_bin;
}

bool MariaDBServer::is_master() const
{
    return status_is_master(m_server_base->pending_status);
}

bool MariaDBServer::is_slave() const
{
    return status_is_slave(m_server_base->pending_status);
}

bool MariaDBServer::is_slave_of_ext_master() const
{
    return status_is_slave_of_ext_master(m_server_base->pending_status);
}

bool MariaDBServer::is_usable() const
{
    return status_is_usable(m_server_base->pending_status);
}

bool MariaDBServer::is_running() const
{
    return status_is_running(m_server_base->pending_status);
}

bool MariaDBServer::is_down() const
{
    return status_is_down(m_server_base->pending_status);
}

bool MariaDBServer::is_in_maintenance() const
{
    return status_is_in_maint(m_server_base->pending_status);
}

bool MariaDBServer::is_relay_master() const
{
    return status_is_relay(m_server_base->pending_status);
}

bool MariaDBServer::is_low_on_disk_space() const
{
    return status_is_disk_space_exhausted(m_server_base->pending_status);
}

bool MariaDBServer::has_status(uint64_t bits) const
{
    return (m_server_base->pending_status & bits) == bits;
}

bool MariaDBServer::had_status(uint64_t bits) const
{
    return (m_server_base->mon_prev_status & bits) == bits;
}

bool MariaDBServer::is_read_only() const
{
    return m_read_only;
}

const char* MariaDBServer::name() const
{
    return m_server_base->server->name;
}

string MariaDBServer::diagnostics() const
{
    // Format strings.
    const char fmt_string[] = "%-23s %s\n";
    const char fmt_int[] = "%-23s %i\n";
    const char fmt_int64[] = "%-23s %" PRIi64 "\n";

    string rval;
    rval.reserve(300); // Enough for most common ouput.

    rval += string_printf(fmt_string, "Server:", name());
    rval += string_printf(fmt_int64, "Server ID:", m_server_id);
    rval += string_printf(fmt_string, "Read only:", (m_read_only ? "Yes" : "No"));
    Guard guard(m_arraylock);
    if (!m_gtid_current_pos.empty())
    {
        rval += string_printf(fmt_string, "Gtid current position:", m_gtid_current_pos.to_string().c_str());
    }
    if (!m_gtid_binlog_pos.empty())
    {
        rval += string_printf(fmt_string, "Gtid binlog position:", m_gtid_binlog_pos.to_string().c_str());
    }
    if (m_node.cycle != NodeData::CYCLE_NONE)
    {
        rval += string_printf(fmt_int, "Master group:", m_node.cycle);
    }

    rval += (m_slave_status.empty() ? "No slave connections\n" : "Slave connections:\n");
    for (const SlaveStatus& sstatus : m_slave_status)
    {
        rval += sstatus.to_string() + "\n";
    }
    return rval;
}

json_t* MariaDBServer::to_json() const
{
    json_t* result = json_object();
    json_object_set_new(result, "name", json_string(name()));
    json_object_set_new(result, "server_id", json_integer(m_server_id));
    json_object_set_new(result, "read_only", json_boolean(m_read_only));

    Guard guard(m_arraylock);
    json_object_set_new(result,
                        "gtid_current_pos",
                        m_gtid_current_pos.empty() ? json_null() :
                        json_string(m_gtid_current_pos.to_string().c_str()));

    json_object_set_new(result,
                        "gtid_binlog_pos",
                        m_gtid_binlog_pos.empty() ? json_null() :
                        json_string(m_gtid_binlog_pos.to_string().c_str()));

    json_object_set_new(result,
                        "master_group",
                        (m_node.cycle == NodeData::CYCLE_NONE) ? json_null() : json_integer(m_node.cycle));

    json_t* slave_connections = json_array();
    for (const auto& sstatus : m_slave_status)
    {
        json_array_append_new(slave_connections, sstatus.to_json());
    }
    json_object_set_new(result, "slave_connections", slave_connections);
    return result;
}

bool MariaDBServer::can_replicate_from(MariaDBServer* master, string* reason_out)
{
    mxb_assert(reason_out);
    mxb_assert(is_usable()); // The server must be running.

    bool can_replicate = false;
    if (m_gtid_current_pos.empty())
    {
        *reason_out = string_printf("'%s' does not have a valid gtid_current_pos.", name());
    }
    else if (master->m_gtid_binlog_pos.empty())
    {
        *reason_out = string_printf("'%s' does not have a valid gtid_binlog_pos.", master->name());
    }
    else
    {
        can_replicate = m_gtid_current_pos.can_replicate_from(master->m_gtid_binlog_pos);
        if (!can_replicate)
        {
            *reason_out = string_printf("gtid_current_pos of '%s' (%s) is incompatible with "
                                        "gtid_binlog_pos of '%s' (%s).",
                                        name(), m_gtid_current_pos.to_string().c_str(),
                                        master->name(), master->m_gtid_binlog_pos.to_string().c_str());
        }
    }
    return can_replicate;
}

bool MariaDBServer::redirect_one_slave(const string& change_cmd)
{
    bool success = false;
    MYSQL* slave_conn = m_server_base->con;
    const char* query = "STOP SLAVE;";
    if (mxs_mysql_query(slave_conn, query) == 0)
    {
        query = "RESET SLAVE;";     // To erase any old I/O or SQL errors
        if (mxs_mysql_query(slave_conn, query) == 0)
        {
            query = "CHANGE MASTER TO ...";     // Don't show the real query as it contains a password.
            if (mxs_mysql_query(slave_conn, change_cmd.c_str()) == 0)
            {
                query = "START SLAVE;";
                if (mxs_mysql_query(slave_conn, query) == 0)
                {
                    success = true;
                    MXS_NOTICE("Slave '%s' redirected to new master.", name());
                }
            }
        }
    }

    if (!success)
    {
        MXS_WARNING("Slave '%s' redirection failed: '%s'. Query: '%s'.",
                    name(), mysql_error(slave_conn), query);
    }
    return success;
}

bool MariaDBServer::run_sql_from_file(const string& path, json_t** error_out)
{
    MYSQL* conn = m_server_base->con;
    bool error = false;
    std::ifstream sql_file(path);
    if (sql_file.is_open())
    {
        MXS_NOTICE("Executing sql queries from file '%s' on server '%s'.", path.c_str(), name());
        int lines_executed = 0;

        while (!sql_file.eof() && !error)
        {
            string line;
            std::getline(sql_file, line);
            if (sql_file.bad())
            {
                PRINT_MXS_JSON_ERROR(error_out,
                                     "Error when reading sql text file '%s': '%s'.",
                                     path.c_str(),
                                     mxs_strerror(errno));
                error = true;
            }
            // Skip empty lines and comment lines
            else if (!line.empty() && line[0] != '#')
            {
                if (mxs_mysql_query(conn, line.c_str()) == 0)
                {
                    lines_executed++;
                    // Discard results if any.
                    MYSQL_RES* res = mysql_store_result(conn);
                    if (res != NULL)
                    {
                        mysql_free_result(res);
                    }
                }
                else
                {
                    PRINT_MXS_JSON_ERROR(error_out,
                                         "Failed to execute sql from text file '%s'. Query: '%s'. "
                                         "Error: '%s'.",
                                         path.c_str(),
                                         line.c_str(),
                                         mysql_error(conn));
                    error = true;
                }
            }
        }
        MXS_NOTICE("%d queries executed successfully.", lines_executed);
    }
    else
    {
        PRINT_MXS_JSON_ERROR(error_out, "Could not open sql text file '%s'.", path.c_str());
        error = true;
    }
    return !error;
}

void MariaDBServer::monitor_server()
{
    string errmsg;
    bool query_ok = false;
    /* Query different things depending on server version/type. */
    if (m_srv_type == server_type::BINLOG_ROUTER)
    {
        // TODO: Add special version of server variable query.
        query_ok = update_slave_status(&errmsg);
    }
    else if (m_capabilities.basic_support)
    {
        query_ok = read_server_variables(&errmsg) && update_slave_status(&errmsg);
        if (query_ok && m_capabilities.gtid)
        {
            query_ok = update_gtids(&errmsg);
        }
        if (query_ok && m_query_events)
        {
            query_ok = update_enabled_events();
        }
    }
    else
    {
        // Not a binlog server and no normal support, don't update.
        query_ok = true;
    }

    if (query_ok)
    {
        m_print_update_errormsg = true;
    }
    /* If one of the queries ran to an error, print the error message, assuming it hasn't already been
     * printed. Some really unlikely errors won't produce an error message, but these are visible in other
     * ways. */
    else if (!errmsg.empty() && m_print_update_errormsg)
    {
        MXS_WARNING("Error during monitor update of server '%s': %s", name(), errmsg.c_str());
        m_print_update_errormsg = false;
    }
    return;
}

/**
 * Update slave status of the server.
 *
 * @param errmsg_out Where to store an error message if query fails. Can be null.
 * @return True on success
 */
bool MariaDBServer::update_slave_status(string* errmsg_out)
{
    bool rval = do_show_slave_status(errmsg_out);
    if (rval)
    {
        /** Store master_id of current node. */
        m_server_base->server->master_id = !m_slave_status.empty() ?
            m_slave_status[0].master_server_id : SERVER_ID_UNKNOWN;
    }
    return rval;
}

void MariaDBServer::update_server_version()
{
    m_srv_type = server_type::UNKNOWN;
    auto conn = m_server_base->con;
    auto srv = m_server_base->server;

    /* Get server version string, also get/set numeric representation. This function does not query the
     * server, since the data was obtained when connecting. */
    mxs_mysql_update_server_version(conn, srv);

    // Check whether this server is a MaxScale Binlog Server.
    MYSQL_RES* result;
    if (mxs_mysql_query(conn, "SELECT @@maxscale_version") == 0
        && (result = mysql_store_result(conn)) != NULL)
    {
        m_srv_type = server_type::BINLOG_ROUTER;
        mysql_free_result(result);
    }
    else
    {
        /* Not a binlog server, check version number and supported features. */
        m_srv_type = server_type::NORMAL;
        m_capabilities = Capabilities();
        SERVER_VERSION decoded = {0, 0, 0};
        server_decode_version(server_get_version(srv), &decoded);
        auto major = decoded.major;
        auto minor = decoded.minor;
        auto patch = decoded.patch;
        // MariaDB/MySQL 5.5 is the oldest supported version. MySQL 6 and later are treated as 5.5.
        if ((major == 5 && minor >= 5) || major > 5)
        {
            m_capabilities.basic_support = true;
            // For more specific features, at least MariaDB 10.X is needed.
            if (srv->server_type == SERVER_TYPE_MARIADB && major >= 10)
            {
                // 10.0.2 or 10.1.X or greater than 10
                if (((minor == 0 && patch >= 2)  || minor >= 1) || major > 10)
                {
                    m_capabilities.gtid = true;
                }
                // 10.1.2 (10.1.1 has limited support, not enough) or 10.2.X or greater than 10
                if (((minor == 1 && patch >= 2)  || minor >= 2) || major > 10)
                {
                    m_capabilities.max_statement_time = true;
                }
            }
        }
        else
        {
            MXS_ERROR("MariaDB/MySQL version of '%s' (%s) is less than 5.5, which is not supported. "
                      "The server is ignored by the monitor.", name(), srv->version_string);
        }
    }
}

void MariaDBServer::check_permissions()
{
    // Test with a typical query to make sure the monitor has sufficient permissions.
    const string query = "SHOW SLAVE STATUS;";
    string err_msg;
    auto result = execute_query(query, &err_msg);

    if (result.get() == NULL)
    {
        /* In theory, this could be due to other errors as well, but that is quite unlikely since the
         * connection was just checked. The end result is in any case that the server is not updated,
         * and that this test is retried next round. */
        set_status(SERVER_AUTH_ERROR);
        // Only print error if last round was ok.
        if (!had_status(SERVER_AUTH_ERROR))
        {
            MXS_WARNING("Error during monitor permissions test for server '%s': %s", name(), err_msg.c_str());
        }
    }
    else
    {
        clear_status(SERVER_AUTH_ERROR);
    }
}

void MariaDBServer::clear_status(uint64_t bits)
{
    monitor_clear_pending_status(m_server_base, bits);
}

void MariaDBServer::set_status(uint64_t bits)
{
    monitor_set_pending_status(m_server_base, bits);
}

/**
 * Compare if the given slave status array is equal to the one stored in the MariaDBServer.
 * Only compares the parts relevant for building replication topology: slave IO/SQL state,
 * host:port and master server id:s. When unsure, return false. This must match
 * 'build_replication_graph()' in the monitor class.
 *
 * @param new_slave_status Right hand side
 * @return True if equal
 */
bool MariaDBServer::sstatus_array_topology_equal(const SlaveStatusArray& new_slave_status)
{
    bool rval = true;
    const SlaveStatusArray& old_slave_status = m_slave_status;
    if (old_slave_status.size() != new_slave_status.size())
    {
        rval = false;
    }
    else
    {
        for (size_t i = 0; i < old_slave_status.size(); i++)
        {
            const auto new_row = new_slave_status[i];
            const auto old_row = old_slave_status[i];
            // Strictly speaking, the following should depend on the 'assume_unique_hostnames',
            // but the situations it would make a difference are so rare they can be ignored.
            if (new_row.slave_io_running != old_row.slave_io_running
                || new_row.slave_sql_running != old_row.slave_sql_running
                || new_row.master_host != old_row.master_host || new_row.master_port != old_row.master_port
                || new_row.master_server_id != old_row.master_server_id)
            {
                rval = false;
                break;
            }
        }
    }
    return rval;
}

/**
 * Check the slave status array stored in the MariaDBServer and find the row matching the connection in
 * 'search_row'.
 *
 * @param search_row What connection to search for
 * @param guess_ind Index where the search row could be located at. If incorrect, the array is searched.
 * @return The found row or NULL if not found
 */
const SlaveStatus* MariaDBServer::sstatus_find_previous_row(const SlaveStatus& search_row, size_t guess_ind)
{
    // Helper function. Checks if the connection in the new row is to the same server than in the old row.
    auto compare_rows = [](const SlaveStatus& lhs, const SlaveStatus& rhs) -> bool {
            return rhs.master_host == lhs.master_host && rhs.master_port == lhs.master_port;
        };

    // Usually the same slave connection can be found from the same index than in the previous slave
    // status array, but this is not 100% (e.g. dba has just added a new connection).
    const SlaveStatus* rval = NULL;
    if (guess_ind < m_slave_status.size() && compare_rows(m_slave_status[guess_ind], search_row))
    {
        rval = &m_slave_status[guess_ind];
    }
    else
    {
        // The correct connection was not found where it should have been. Try looping.
        for (const SlaveStatus& old_row : m_slave_status)
        {
            if (compare_rows(old_row, search_row))
            {
                rval = &old_row;
                break;
            }
        }
    }
    return rval;
}

bool MariaDBServer::can_be_demoted_switchover(string* reason_out)
{
    bool demotable = false;
    string reason;
    string query_error;

    if (!is_usable())
    {
        reason = "it is not running or it is in maintenance.";
    }
    else if (!update_replication_settings(&query_error))
    {
        reason = string_printf("it could not be queried: %s", query_error.c_str());
    }
    else if (!binlog_on())
    {
        reason = "its binary log is disabled.";
    }
    else if (!is_master() && !m_rpl_settings.log_slave_updates)
    {
        // This means that gtid_binlog_pos cannot be trusted.
        // TODO: reduce dependency on gtid_binlog_pos to get rid of this requirement
        reason = "it is not the master and log_slave_updates is disabled.";
    }
    else if (m_gtid_binlog_pos.empty())
    {
        reason = "it does not have a 'gtid_binlog_pos'.";
    }
    else
    {
        demotable = true;
    }

    if (!demotable && reason_out)
    {
        *reason_out = reason;
    }
    return demotable;
}

bool MariaDBServer::can_be_demoted_failover(string* reason_out)
{
    bool demotable = false;
    string reason;

    if (is_master())
    {
        reason = "it is a running master.";
    }
    else if (is_running())
    {
        reason = "it is running.";
    }
    else if (m_gtid_binlog_pos.empty())
    {
        reason = "it does not have a 'gtid_binlog_pos'.";
    }
    else
    {
        demotable = true;
    }

    if (!demotable && reason_out)
    {
        *reason_out = reason;
    }
    return demotable;
}

bool MariaDBServer::can_be_promoted(OperationType op, const MariaDBServer* demotion_target,
                                    string* reason_out)
{
    bool promotable = false;
    string reason;
    string query_error;

    auto sstatus = slave_connection_status(demotion_target);
    if (is_master())
    {
        reason = "it is already the master.";
    }
    else if (!is_usable())
    {
        reason = "it is down or in maintenance.";
    }
    else if (op == OperationType::SWITCHOVER && is_low_on_disk_space())
    {
        // Failover promotion with low disk space is allowed since it's better than nothing.
        reason = "it is low on disk space.";
    }
    else if (sstatus == NULL)
    {
        reason = string_printf("it is not replicating from '%s'.", demotion_target->name());
    }
    else if (sstatus->gtid_io_pos.empty())
    {
        reason = string_printf("its slave connection to '%s' is not using gtid.", demotion_target->name());
    }
    else if (op == OperationType::SWITCHOVER && sstatus->slave_io_running != SlaveStatus::SLAVE_IO_YES)
    {
        reason = string_printf("its slave connection to '%s' is broken.", demotion_target->name());
    }
    else if (!update_replication_settings(&query_error))
    {
        reason = string_printf("it could not be queried: %s", query_error.c_str());
    }
    else if (!binlog_on())
    {
        reason = "its binary log is disabled.";
    }
    else
    {
        promotable = true;
    }

    if (!promotable && reason_out)
    {
        *reason_out = reason;
    }
    return promotable;
}

const SlaveStatus* MariaDBServer::slave_connection_status(const MariaDBServer* target) const
{
    // The slave node may have several slave connections, need to find the one that is
    // connected to the parent. TODO: Use the information gathered in 'build_replication_graph'
    // to skip this function, as the contents are very similar.
    const SlaveStatus* rval = NULL;
    if (m_assume_unique_hostnames)
    {
        // Can simply compare host:port.
        SERVER* target_srv = target->m_server_base->server;
        string target_host = target_srv->address;
        int target_port = target_srv->port;
        for (const SlaveStatus& ss : m_slave_status)
        {
            if (ss.master_host == target_host && ss.master_port == target_port &&
                ss.slave_sql_running && ss.slave_io_running != SlaveStatus::SLAVE_IO_NO)
            {
                rval = &ss;
                break;
            }
        }
    }
    else
    {
        // Compare server id:s instead. If the master's id is wrong (e.g. never updated) this gives a
        // wrong result. Also gives wrong result if monitor has never seen the slave connection in the
        // connected state.
        auto target_id = target->m_server_id;
        for (const SlaveStatus& ss : m_slave_status)
        {
            auto master_id = ss.master_server_id;
            if (master_id > 0 && master_id == target_id && ss.slave_sql_running && ss.seen_connected
                && ss.slave_io_running != SlaveStatus::SLAVE_IO_NO)
            {
                rval = &ss;
                break;
            }
        }
    }
    return rval;
}

const SlaveStatus* MariaDBServer::slave_connection_status_host_port(const MariaDBServer* target) const
{
    string target_host = target->m_server_base->server->address;
    int target_port = target->m_server_base->server->port;
    for (const SlaveStatus& ss : m_slave_status)
    {
        if (ss.master_host == target_host && ss.master_port == target_port)
        {
            return &ss;
        }
    }
    return NULL;
}

bool MariaDBServer::enable_events(const EventNameSet& event_names, json_t** error_out)
{
    int found_disabled_events = 0;
    int events_enabled = 0;

    // Helper function which enables a disabled event if that event name is found in the events-set.
    ManipulatorFunc enabler = [this, event_names, &found_disabled_events, &events_enabled](
            const EventInfo& event, json_t** error_out) {
        if (event_names.count(event.name) > 0
            && (event.status == "SLAVESIDE_DISABLED" || event.status == "DISABLED"))
        {
            found_disabled_events++;
            if (alter_event(event, "ENABLE", error_out))
            {
                events_enabled++;
            }
        }
    };

    bool rval = false;
    if (events_foreach(enabler, error_out))
    {
        if (found_disabled_events > 0)
        {
            warn_event_scheduler();
        }
        if (found_disabled_events == events_enabled)
        {
            rval = true;
        }
    }
    return rval;
}

bool MariaDBServer::disable_events(BinlogMode binlog_mode, json_t** error_out)
{
    int found_enabled_events = 0;
    int events_disabled = 0;
    // Helper function which disables an enabled event.
    ManipulatorFunc disabler = [this, &found_enabled_events, &events_disabled](const EventInfo& event,
                                                                               json_t** error_out) {
            if (event.status == "ENABLED")
            {
                found_enabled_events++;
                if (alter_event(event, "DISABLE ON SLAVE", error_out))
                {
                    events_disabled++;
                }
            }
        };

    // If the server is rejoining the cluster, no events may be added to binlog. The ALTER EVENT query
    // itself adds events. To prevent this, disable the binlog for this method.
    string error_msg;
    if (binlog_mode == BinlogMode::BINLOG_OFF)
    {
        if (!execute_cmd("SET @@session.sql_log_bin=0;", &error_msg))
        {
            const char FMT[] = "Could not disable session binlog on '%s': %s Server events not disabled.";
            PRINT_MXS_JSON_ERROR(error_out, FMT, name(), error_msg.c_str());
            return false;
        }
    }

    bool rval = false;
    if (events_foreach(disabler, error_out))
    {
        if (found_enabled_events > 0)
        {
            warn_event_scheduler();
        }
        if (found_enabled_events == events_disabled)
        {
            rval = true;
        }
    }

    if (binlog_mode == BinlogMode::BINLOG_OFF)
    {
        // Failure in re-enabling the session binlog doesn't really matter because we don't want the monitor
        // generating binlog events anyway.
        execute_cmd("SET @@session.sql_log_bin=1;");
    }
    return rval;
    // TODO: For better error handling, this function should try to re-enable any disabled events if a later
    // disable fails.
}

/**
 * Print a warning if the event scheduler is off.
 */
void MariaDBServer::warn_event_scheduler()
{
    string error_msg;
    const string scheduler_query = "SELECT * FROM information_schema.PROCESSLIST "
                                   "WHERE User = 'event_scheduler' AND Command = 'Daemon';";
    auto proc_list = execute_query(scheduler_query, &error_msg);
    if (proc_list.get() == NULL)
    {
        MXS_ERROR("Could not query the event scheduler status of '%s': %s", name(), error_msg.c_str());
    }
    else
    {
        if (proc_list->get_row_count() < 1)
        {
            // This is ok, though unexpected since events were found.
            MXS_WARNING("Event scheduler is inactive on '%s' although events were found.", name());
        }
    }
}

/**
 * Run the manipulator function on every server event.
 *
 * @param func The manipulator function
 * @param error_out Error output
 * @return True if event information could be read from information_schema.EVENTS. The return value does not
 * depend on the manipulator function.
 */
bool MariaDBServer::events_foreach(ManipulatorFunc& func, json_t** error_out)
{
    string error_msg;
    // Get info about all scheduled events on the server.
    auto event_info = execute_query("SELECT * FROM information_schema.EVENTS;", &error_msg);
    if (event_info.get() == NULL)
    {
        MXS_ERROR("Could not query event status of '%s': %s Event handling can be disabled by "
                  "setting '%s' to false.",
                  name(), error_msg.c_str(), CN_HANDLE_EVENTS);
        return false;
    }

    auto db_name_ind = event_info->get_col_index("EVENT_SCHEMA");
    auto event_name_ind = event_info->get_col_index("EVENT_NAME");
    auto event_definer_ind = event_info->get_col_index("DEFINER");
    auto event_status_ind = event_info->get_col_index("STATUS");
    mxb_assert(db_name_ind > 0 && event_name_ind > 0 && event_definer_ind > 0 && event_status_ind > 0);

    while (event_info->next_row())
    {
        EventInfo event;
        event.name = event_info->get_string(db_name_ind) + "." + event_info->get_string(event_name_ind);
        event.definer = event_info->get_string(event_definer_ind);
        event.status = event_info->get_string(event_status_ind);
        func(event, error_out);
    }
    return true;
}

/**
 * Alter a scheduled server event, setting its status.
 *
 * @param event Event to alter
 * @param target_status Status to set
 * @param error_out Error output
 * @return True if status was set
 */
bool MariaDBServer::alter_event(const EventInfo& event, const string& target_status, json_t** error_out)
{
    bool rval = false;
    string error_msg;
    // An ALTER EVENT by default changes the definer (owner) of the event to the monitor user.
    // This causes problems if the monitor user does not have privileges to run
    // the event contents. Prevent this by setting definer explicitly.
    // The definer may be of the form user@host. If host includes %, then it must be quoted.
    // For simplicity, quote the host always.
    string quoted_definer;
    auto loc_at = event.definer.find('@');
    if (loc_at != string::npos)
    {
        auto host_begin = loc_at + 1;
        quoted_definer = event.definer.substr(0, loc_at + 1)
            +   // host_begin may be the null-char if @ was the last char
            "'" + event.definer.substr(host_begin, string::npos) + "'";
    }
    else
    {
        // Just the username
        quoted_definer = event.definer;
    }

    string alter_event_query = string_printf("ALTER DEFINER = %s EVENT %s %s;",
                                             quoted_definer.c_str(),
                                             event.name.c_str(),
                                             target_status.c_str());
    if (execute_cmd(alter_event_query, &error_msg))
    {
        rval = true;
        const char FMT[] = "Event '%s' on server '%s' set to '%s'.";
        MXS_NOTICE(FMT, event.name.c_str(), name(), target_status.c_str());
    }
    else
    {
        const char FMT[] = "Could not alter event '%s' on server '%s': %s";
        PRINT_MXS_JSON_ERROR(error_out, FMT, event.name.c_str(), name(), error_msg.c_str());
    }
    return rval;
}

bool MariaDBServer::reset_all_slave_conns(json_t** error_out)
{
    string error_msg;
    bool error = false;
    for (auto& sstatus : m_slave_status)
    {
        auto stop = string_printf("STOP SLAVE '%s';", sstatus.name.c_str());
        auto reset = string_printf("RESET SLAVE '%s' ALL;", sstatus.name.c_str());
        if (!execute_cmd(stop, &error_msg) || !execute_cmd(reset, &error_msg))
        {
            error = true;
            string log_message = sstatus.name.empty() ?
                string_printf("Error when reseting the default slave connection of '%s': %s",
                              name(), error_msg.c_str()) :
                string_printf("Error when reseting the slave connection '%s' of '%s': %s",
                              sstatus.name.c_str(), name(), error_msg.c_str());
            PRINT_MXS_JSON_ERROR(error_out, "%s", log_message.c_str());
            break;
        }
    }

    if (!error && !m_slave_status.empty())
    {
        MXS_NOTICE("Removed %lu slave connection(s) from '%s'.", m_slave_status.size(), name());
    }
    return !error;
}

bool MariaDBServer::promote(GeneralOpData& general, ServerOperation& promotion, OperationType type,
                            const MariaDBServer* demotion_target)
{
    mxb_assert(type == OperationType::SWITCHOVER || type == OperationType::FAILOVER);
    json_t** const error_out = general.error_out;
    // Function should only be called for a master-slave pair.
    auto master_conn = slave_connection_status(demotion_target);
    mxb_assert(master_conn);
    if (master_conn == NULL)
    {
        PRINT_MXS_JSON_ERROR(error_out,
                             "'%s' is not a slave of '%s' and cannot be promoted to its place.",
                             name(), demotion_target->name());
        return false;
    }

    bool success = false;
    StopWatch timer;
    // Step 1: Stop & reset slave connections. If doing a failover, only remove the connection to demotion
    // target. In case of switchover, remove other slave connections as well since the demotion target
    // will take them over.
    bool stopped = false;
    if (type == OperationType::SWITCHOVER)
    {
        stopped = remove_slave_conns(general, m_slave_status);
    }
    else if (type == OperationType::FAILOVER)
    {
        stopped = remove_slave_conns(general, {*master_conn});
        master_conn = NULL; // The connection pointed to may no longer exist.
    }

    if (stopped)
    {
        // Step 2: If demotion target is master, meaning this server will become the master,
        // enable writing and scheduled events. Also, run promotion_sql_file.
        bool promotion_error = false;
        if (promotion.to_from_master)
        {
            // Disabling read-only should be quick.
            bool ro_disabled = set_read_only(ReadOnlySetting::DISABLE, general.time_remaining, error_out);
            general.time_remaining -= timer.restart();
            if (!ro_disabled)
            {
                promotion_error = true;
            }
            else
            {
                if (promotion.handle_events)
                {
                    // TODO: Add query replying to enable_events
                    bool events_enabled = enable_events(promotion.events_to_enable, error_out);
                    general.time_remaining -= timer.restart();
                    if (!events_enabled)
                    {
                        promotion_error = true;
                        PRINT_MXS_JSON_ERROR(error_out, "Failed to enable events on '%s'.", name());
                    }
                }

                // Run promotion_sql_file if no errors so far.
                const string& sql_file = promotion.sql_file;
                if (!promotion_error && !sql_file.empty())
                {
                    bool file_ran_ok = run_sql_from_file(sql_file, error_out);
                    general.time_remaining -= timer.restart();
                    if (!file_ran_ok)
                    {
                        promotion_error = true;
                        PRINT_MXS_JSON_ERROR(error_out,
                                             "Execution of file '%s' failed during promotion of server '%s'.",
                                             sql_file.c_str(), name());
                    }
                }
            }
        }

        // Step 3: Copy or merge slave connections from demotion target. The logic used depends on the
        // operation.
        if (!promotion_error)
        {
            if (type == OperationType::SWITCHOVER)
            {
                if (copy_slave_conns(general, promotion.conns_to_copy, demotion_target))
                {
                    success = true;
                }
                else
                {
                    PRINT_MXS_JSON_ERROR(error_out, "Could not copy slave connections from '%s' to '%s'.",
                                         demotion_target->name(), name());
                }
            }
            else if (type == OperationType::FAILOVER)
            {
                if (merge_slave_conns(general, promotion.conns_to_copy))
                {
                    success = true;
                }
                else
                {
                    PRINT_MXS_JSON_ERROR(error_out, "Could not merge slave connections from '%s' to '%s'.",
                                         demotion_target->name(), name());
                }
            }
        }
    }
    return success;
}

bool MariaDBServer::demote(GeneralOpData& general, ServerOperation& demotion)
{
    mxb_assert(demotion.target == this);
    json_t** const error_out = general.error_out;
    bool success = false;

    // Step 1: Stop & reset slave connections. The promotion target will copy them. The connection
    // information has been backed up in the operation object.
    if (remove_slave_conns(general, m_slave_status))
    {
        // Step 2: If this server is master, disable writes and scheduled events, flush logs,
        // update gtid:s, run demotion_sql_file.

        // In theory, this part should be ran in the opposite order so it would "reverse"
        // the promotion code. However, it's probably better to run the most
        // likely part to fail, setting read_only=1, first to make undoing easier. Setting
        // read_only may fail if another session has table locks or is doing long writes.
        bool demotion_error = false;
        if (demotion.to_from_master)
        {
            // The server should either be the master or be a standalone being rejoined.
            mxb_assert(is_master() || m_slave_status.empty());

            // Step 2a: Remove [Master] from this server. This prevents compatible routers (RWS)
            // from routing writes to this server. Writes in flight will go through, at least until
            // read_only is set.
            clear_status(SERVER_MASTER);
            // Step 2b: If other users with SUPER privileges are on, kick them out now since
            // read_only doesn't stop them from doing writes. This does not stop them from immediately
            // logging back in but it's better than nothing. This also stops super-user writes going
            // through MaxScale.
            if (!kick_out_super_users(general))
            {
                demotion_error = true;
            }

            // Step 2c: Enabling read-only can take time if writes are on or table locks taken.
            StopWatch timer;
            if (!demotion_error)
            {
                bool ro_enabled = set_read_only(ReadOnlySetting::ENABLE, general.time_remaining, error_out);
                general.time_remaining -= timer.lap();
                if (!ro_enabled)
                {
                    demotion_error = true;
                }
            }

            if (!demotion_error && demotion.handle_events)
            {
                // TODO: Add query replying to enable_events
                // Step 2d: Using BINLOG_OFF to avoid adding any gtid events,
                // which could break external replication.
                bool events_disabled = disable_events(BinlogMode::BINLOG_OFF, error_out);
                general.time_remaining -= timer.lap();
                if (!events_disabled)
                {
                    demotion_error = true;
                    PRINT_MXS_JSON_ERROR(error_out, "Failed to disable events on '%s'.", name());
                }
            }

            // Step 2e: Run demotion_sql_file if no errors so far.
            const string& sql_file = demotion.sql_file;
            if (!demotion_error && !sql_file.empty())
            {
                bool file_ran_ok = run_sql_from_file(sql_file, error_out);
                general.time_remaining -= timer.lap();
                if (!file_ran_ok)
                {
                    demotion_error = true;
                    PRINT_MXS_JSON_ERROR(error_out,
                                         "Execution of file '%s' failed during demotion of server '%s'.",
                                         sql_file.c_str(), name());
                }
            }

            if (!demotion_error)
            {
                // Step 2f: FLUSH LOGS to ensure that all events have been written to binlog.
                string error_msg;
                bool logs_flushed = execute_cmd_time_limit("FLUSH LOGS;", general.time_remaining,
                                                           &error_msg);
                general.time_remaining -= timer.lap();
                if (!logs_flushed)
                {
                    demotion_error = true;
                    PRINT_MXS_JSON_ERROR(error_out,
                                         "Failed to flush binary logs of '%s' during demotion: %s.",
                                         name(), error_msg.c_str());
                }
            }
        }

        if (!demotion_error)
        {
            // Finally, update gtid:s.
            string error_msg;
            if (update_gtids(&error_msg))
            {
                success = true;
            }
            else
            {
                demotion_error = true;
                PRINT_MXS_JSON_ERROR(error_out, "Failed to update gtid:s of '%s' during demotion: %s.",
                                     name(), error_msg.c_str());
            }
        }

        if (demotion_error && demotion.to_from_master)
        {
            // Read_only was enabled (or tried to be enabled) but a later step failed.
            // Disable read_only. Connection is likely broken so use a short time limit.
            // Even this is insufficient, because the server may still be executing the old
            // 'SET GLOBAL read_only=1' query.
            // TODO: add smarter undo, KILL QUERY etc.
            set_read_only(ReadOnlySetting::DISABLE, Duration((double)0), NULL);
        }
    }
    return success;
}

/**
 * Stop and optionally reset/reset-all a slave connection.
 *
 * @param conn_name Slave connection name. Use empty string for the nameless connection.
 * @param mode STOP, RESET or RESET ALL
 * @param time_limit Operation time limit
 * @param error_out Error output
 * @return True on success
 */
bool MariaDBServer::stop_slave_conn(const std::string& conn_name, StopMode mode, Duration time_limit,
                                    json_t** error_out)
{
    /* STOP SLAVE is a bit problematic, since sometimes it seems to take several seconds to complete.
     * If this time is greater than the connection read timeout, connector-c will cut the connection/
     * query. The query is likely completed afterwards by the server. To prevent false errors,
     * try the query repeatedly until time is up. Fortunately, the server doesn't consider stopping
     * an already stopped slave connection an error. */
    Duration time_left = time_limit;
    StopWatch timer;
    string stop = string_printf("STOP SLAVE '%s';", conn_name.c_str());
    string error_msg;
    bool stop_success = execute_cmd_time_limit(stop, time_left, &error_msg);
    time_left -= timer.restart();

    bool rval = false;
    if (stop_success)
    {
        // The RESET SLAVE-query can also take a while if there is lots of relay log to delete.
        // Very rare, though.
        if (mode == StopMode::RESET || mode == StopMode::RESET_ALL)
        {
            string reset = string_printf("RESET SLAVE '%s'%s;",
                                         conn_name.c_str(), (mode == StopMode::RESET_ALL) ? " ALL" : "");
            if (execute_cmd_time_limit(reset, time_left, &error_msg))
            {
                rval = true;
            }
            else
            {
                PRINT_MXS_JSON_ERROR(error_out, "Failed to reset slave connection on '%s': %s",
                                     name(), error_msg.c_str());
            }
        }
        else
        {
            rval = true;
        }
    }
    else
    {
        PRINT_MXS_JSON_ERROR(error_out, "Failed to stop slave connection on '%s': %s",
                             name(), error_msg.c_str());
    }
    return rval;
}

/**
 * Removes the given slave connections from the server and then updates slave connection status.
 * The slave connections of the server object will change during this method, so any pointers and
 * references to such may be invalidated and should be re-acquired.
 *
 * @param op Operation descriptor
 * @param conns_to_remove Which connections should be removed
 * @return True if successful
 */
bool MariaDBServer::remove_slave_conns(GeneralOpData& op, const SlaveStatusArray& conns_to_remove)
{
    json_t** error_out = op.error_out;
    maxbase::Duration& time_remaining = op.time_remaining;
    StopWatch timer;
    // Take a backup of the soon to be removed connections so they can be compared properly after an update.
    SlaveStatusArray conns_to_remove_copy = conns_to_remove;

    bool stop_slave_error = false;
    for (size_t i = 0; !stop_slave_error && i < conns_to_remove.size(); i++)
    {
        if (!stop_slave_conn(conns_to_remove[i].name, StopMode::RESET_ALL, time_remaining, error_out))
        {
            stop_slave_error = true;
        }
        time_remaining -= timer.lap();
    }

    bool success = false;
    if (stop_slave_error)
    {
        PRINT_MXS_JSON_ERROR(error_out, "Failed to remove slave connection(s) from '%s'.", name());
    }
    else
    {
        // Check that the slave connections are really gone by comparing connection names. It's probably
        // enough to just update the slave status. Checking that the connections are really gone is
        // likely overkill, but doesn't hurt.
        string error_msg;
        if (do_show_slave_status(&error_msg))
        {
            // Insert all existing connection names to a set, then check that none of the removed ones are
            // there.
            std::set<string> connection_names;
            for (auto& slave_conn : m_slave_status)
            {
                connection_names.insert(slave_conn.name);
            }
            int found = 0;
            for (auto& removed_conn : conns_to_remove_copy)
            {
                if (connection_names.count(removed_conn.name) > 0)
                {
                    found++;
                }
            }

            if (found == 0)
            {
                success = true;
            }
            else
            {
                // This means server is really bugging.
                PRINT_MXS_JSON_ERROR(error_out,
                                     "'%s' still has %i removed slave connections, "
                                     "RESET SLAVE must have failed.", name(), found);
            }
        }
        else
        {
            PRINT_MXS_JSON_ERROR(error_out, "Failed to update slave connections of '%s': %s",
                                 name(), error_msg.c_str());
        }
    }
    time_remaining -= timer.lap();
    return success;
}

bool MariaDBServer::set_read_only(ReadOnlySetting setting, maxbase::Duration time_limit, json_t** error_out)
{
    int new_val = (setting == ReadOnlySetting::ENABLE) ? 1 : 0;
    string cmd = string_printf("SET GLOBAL read_only=%i;", new_val);
    string error_msg;
    bool success = execute_cmd_time_limit(cmd, time_limit, &error_msg);
    if (!success)
    {
        string target_str = (setting == ReadOnlySetting::ENABLE) ? "enable" : "disable";
        PRINT_MXS_JSON_ERROR(error_out,
                             "Failed to %s read_only on '%s': %s",
                             target_str.c_str(), name(), error_msg.c_str());
    }
    return success;
}

/**
 * Merge slave connections to this server (promotion target). This should only
 * be used during failover promotion.
 *
 * @param op Operation descriptor
 * @param conns_to_merge Connections which should be merged
 * @return True on success
 */
bool MariaDBServer::merge_slave_conns(GeneralOpData& op, const SlaveStatusArray& conns_to_merge)
{
    /* When promoting a server during failover, the situation is more complicated than in switchover.
     * Connections cannot be moved to the demotion target (= failed server) as it is off. This means
     * that the promoting server must combine the roles of both itself and the failed server. Only the
     * slave connection replicating from the failed server has been removed. This means that
     * the promotion and demotion targets may have identical connections (connections going to
     * the same server id or the same host:port). These connections should not be copied or modified.
     * It's possible that the master had different settings for a duplicate slave connection,
     * in this case the settings on the master are lost.
     * TODO: think if the master's settings should take priority.
     * Also, connection names may collide between the two servers, in this case try to generate
     * a simple name for the new connection. */

    // Helper function for checking if a slave connection should be ignored.
    auto conn_can_be_merged = [this](const SlaveStatus& slave_conn, string* ignore_reason_out) -> bool {
            bool accepted = true;
            auto master_id = slave_conn.master_server_id;
            string my_host = m_server_base->server->address;
            int my_port = m_server_base->server->port;
            // The connection is only merged if it satisfies the copy-conditions. Merging has also
            // additional requirements.
            string ignore_reason;
            if (!slave_conn.should_be_copied(&ignore_reason))
            {
                accepted = false;
            }
            else if (master_id == m_server_id)
            {
                // This is not an error but indicates a complicated topology. In any case, ignore this.
                accepted = false;
                ignore_reason = string_printf("it points to '%s' (according to server id:s).", name());
            }
            else if (slave_conn.master_host == my_host && slave_conn.master_port == my_port)
            {
                accepted = false;
                ignore_reason = string_printf("it points to '%s' (according to master host:port).", name());
            }
            else
            {
                // Compare to connections already existing on this server.
                for (const SlaveStatus& my_slave_conn : m_slave_status)
                {
                    if (my_slave_conn.seen_connected && my_slave_conn.master_server_id == master_id)
                    {
                        accepted = false;
                        const char format[] = "its Master_Server_Id (%" PRIi64 ") matches an existing "
                                              "slave connection on '%s'.";
                        ignore_reason = string_printf(format, master_id, name());
                    }
                    else if (my_slave_conn.master_host == slave_conn.master_host
                             && my_slave_conn.master_port == slave_conn.master_port)
                    {
                        accepted = false;
                        ignore_reason = string_printf("its Master_Host (%s) and Master_Port (%i) match "
                                                      "an existing slave connection on %s.",
                                                      slave_conn.master_host.c_str(), slave_conn.master_port,
                                                      name());
                    }
                }
            }

            if (!accepted)
            {
                *ignore_reason_out = ignore_reason;
            }
            return accepted;
        };

    // Need to keep track of connection names (both existing and new) to avoid using an existing name.
    std::set<string> connection_names;
    for (const auto& conn : m_slave_status)
    {
        connection_names.insert(conn.name);
    }

    // Helper function which checks that a connection name is unique and modifies it if not.
    auto check_modify_conn_name = [this, &connection_names](SlaveStatus* slave_conn) -> bool {
            bool name_is_unique = false;
            if (connection_names.count(slave_conn->name) > 0)
            {
                // If the name is used, generate a name using the host:port of the master,
                // it should be unique.
                string second_try = string_printf("To [%s]:%i",
                                                  slave_conn->master_host.c_str(), slave_conn->master_port);
                if (connection_names.count(second_try) > 0)
                {
                    // Even this one exists, something is really wrong. Give up.
                    MXS_ERROR("Could not generate a unique connection name for '%s': both '%s' and '%s' are "
                              "already taken.", name(), slave_conn->name.c_str(), second_try.c_str());
                }
                else
                {
                    MXS_WARNING("A slave connection with name '%s' already exists on '%s', using generated "
                                "name '%s' instead.", slave_conn->name.c_str(), name(), second_try.c_str());
                    slave_conn->name = second_try;
                    name_is_unique = true;
                }
            }
            else
            {
                name_is_unique = true;
            }
            return name_is_unique;
        };

    bool error = false;
    for (size_t i = 0; !error && (i < conns_to_merge.size()); i++)
    {
        // Need a copy of the array element here since it may be modified.
        SlaveStatus slave_conn = conns_to_merge[i];
        string ignore_reason;
        if (conn_can_be_merged(slave_conn, &ignore_reason))
        {
            if (check_modify_conn_name(&slave_conn))
            {
                if (create_start_slave(op, slave_conn))
                {
                    connection_names.insert(slave_conn.name);
                }
                else
                {
                    error = true;
                }
            }
            else
            {
                error = true;
            }
        }
        else
        {
            mxb_assert(!ignore_reason.empty());
            MXS_WARNING("%s was ignored when promoting '%s' because %s",
                        slave_conn.to_short_string().c_str(), name(), ignore_reason.c_str());
        }
    }

    return !error;
}

bool MariaDBServer::copy_slave_conns(GeneralOpData& op, const SlaveStatusArray& conns_to_copy,
                                     const MariaDBServer* replacement)
{
    mxb_assert(m_slave_status.empty());
    bool start_slave_error = false;
    for (size_t i = 0; i < conns_to_copy.size() && !start_slave_error; i++)
    {
        SlaveStatus slave_conn = conns_to_copy[i];      // slave_conn may be modified
        string reason_not_copied;
        if (slave_conn.should_be_copied(&reason_not_copied))
        {
            // Any slave connection that was going to this server itself are instead directed
            // to the replacement server.
            if (slave_conn.master_server_id == m_server_id)
            {
                slave_conn.master_host = replacement->m_server_base->server->address;
                slave_conn.master_port = replacement->m_server_base->server->port;
            }
            if (!create_start_slave(op, slave_conn))
            {
                start_slave_error = true;
            }
        }
        else
        {
            MXS_WARNING("%s was not copied to '%s' because %s",
                        slave_conn.to_short_string().c_str(), name(), reason_not_copied.c_str());
        }
    }
    return !start_slave_error;
}

bool MariaDBServer::create_start_slave(GeneralOpData& op, const SlaveStatus& slave_conn)
{
    maxbase::Duration& time_remaining = op.time_remaining;
    StopWatch timer;
    string error_msg;
    bool success = false;
    SlaveStatus new_conn = slave_conn;
    new_conn.owning_server = name();
    string change_master = generate_change_master_cmd(op, new_conn);
    bool conn_created = execute_cmd_time_limit(change_master, time_remaining, &error_msg);
    time_remaining -= timer.restart();
    if (conn_created)
    {
        string start_slave = string_printf("START SLAVE '%s';", new_conn.name.c_str());
        bool slave_started = execute_cmd_time_limit(start_slave, time_remaining, &error_msg);
        time_remaining -= timer.restart();
        if (slave_started)
        {
            success = true;
            MXS_NOTICE("%s created and started.", new_conn.to_short_string().c_str());
        }
        else
        {
            MXS_ERROR("%s could not be started: %s",
                      new_conn.to_short_string().c_str(), error_msg.c_str());
        }
    }
    else
    {
        // TODO: This may currently print out passwords.
        MXS_ERROR("%s could not be created: %s",
                  new_conn.to_short_string().c_str(), error_msg.c_str());
    }
    return success;
}

/**
 * Generate a CHANGE MASTER TO-query.
 *
 * @param op Operation descriptor, required for username and password
 * @param slave_conn Existing slave connection to emulate
 * @return Generated query
 */
string MariaDBServer::generate_change_master_cmd(GeneralOpData& op, const SlaveStatus& slave_conn)
{
    string change_cmd;
    change_cmd += string_printf("CHANGE MASTER '%s' TO MASTER_HOST = '%s', MASTER_PORT = %i, ",
                                slave_conn.name.c_str(),
                                slave_conn.master_host.c_str(), slave_conn.master_port);
    change_cmd += "MASTER_USE_GTID = current_pos, ";
    if (op.replication_ssl)
    {
        change_cmd += "MASTER_SSL = 1, ";
    }
    change_cmd += string_printf("MASTER_USER = '%s', ", op.replication_user.c_str());
    const char MASTER_PW[] = "MASTER_PASSWORD = '%s';";
#if defined (SS_DEBUG)
    string change_cmd_nopw = change_cmd;
    change_cmd_nopw += string_printf(MASTER_PW, "******");
    MXS_DEBUG("Change master command is '%s'.", change_cmd_nopw.c_str());
#endif
    change_cmd += string_printf(MASTER_PW, op.replication_password.c_str());
    return change_cmd;
}

bool MariaDBServer::redirect_existing_slave_conn(GeneralOpData& op, const SlaveStatus& old_conn,
                                                 const MariaDBServer* new_master)
{
    auto error_out = op.error_out;
    maxbase::Duration& time_remaining = op.time_remaining;
    StopWatch timer;
    bool success = false;

    // First, just stop the slave connection.
    bool stopped = stop_slave_conn(old_conn.name, StopMode::STOP_ONLY, time_remaining, error_out);
    time_remaining -= timer.restart();
    if (stopped)
    {
        SlaveStatus modified_conn = old_conn;
        SERVER* target_server = new_master->m_server_base->server;
        modified_conn.master_host = target_server->address;
        modified_conn.master_port = target_server->port;
        string change_master = generate_change_master_cmd(op, modified_conn);
        string error_msg;
        bool changed = execute_cmd_time_limit(change_master, time_remaining, &error_msg);
        time_remaining -= timer.restart();
        if (changed)
        {
            string start = string_printf("START SLAVE '%s';", old_conn.name.c_str());
            bool started = execute_cmd_time_limit(start, time_remaining, &error_msg);
            time_remaining -= timer.restart();
            if (started)
            {
                success = true;
            }
            else
            {
                PRINT_MXS_JSON_ERROR(error_out,
                                     "%s could not be started: %s",
                                     modified_conn.to_short_string().c_str(), error_msg.c_str());
            }
        }
        else
        {
            // TODO: This may currently print out passwords.
            PRINT_MXS_JSON_ERROR(error_out,
                                 "%s could not be redirected to [%s]:%i: %s",
                                 old_conn.to_short_string().c_str(),
                                 modified_conn.master_host.c_str(), modified_conn.master_port,
                                 error_msg.c_str());
        }
    }   // 'stop_slave_conn' prints its own errors
    return success;
}

bool MariaDBServer::update_enabled_events()
{
    string error_msg;
    // Get names of all enabled scheduled events on the server.
    auto event_info = execute_query("SELECT Event_schema, Event_name FROM information_schema.EVENTS WHERE "
                                    "Status = 'ENABLED';", &error_msg);
    if (event_info.get() == NULL)
    {
                MXS_ERROR("Could not query events of '%s': %s Event handling can be disabled by "
                          "setting '%s' to false.",
                          name(), error_msg.c_str(), CN_HANDLE_EVENTS);
        return false;
    }

    auto db_name_ind = 0;
    auto event_name_ind = 1;

    EventNameSet full_names;
    full_names.reserve(event_info->get_row_count());

    while (event_info->next_row())
    {
        string full_name = event_info->get_string(db_name_ind) + "." + event_info->get_string(event_name_ind);
        full_names.insert(full_name); // Ignore duplicates, they shouldn't exists.
    }

    m_enabled_events = std::move(full_names);
    return true;
}

bool MariaDBServer::kick_out_super_users(GeneralOpData& op)
{
    bool error = false;
    Duration time_remaining = op.time_remaining;
    auto error_out = op.error_out;
    // Only select unique rows...
    string get_ids_query = "SELECT DISTINCT * FROM ("
                           // select conn id and username from live connections ...
                           "SELECT P.id,P.user FROM information_schema.PROCESSLIST as P "
                           // match with user information ...
                           "INNER JOIN mysql.user as U ON (U.user = P.user) WHERE "
                           // where the user has super-privileges, is not replicating ...
                           "(U.Super_priv = 'Y' AND P.COMMAND != 'Binlog Dump' "
                           // and is not the current user.
                           "AND P.id != (SELECT CONNECTION_ID()))) as I;";

    string error_msg;
    unsigned int error_num = 0;
    auto res = execute_query(get_ids_query, &error_msg, &error_num);
    if (res)
    {
        int id_col = 0;
        int user_col = 1;
        while (res->next_row())
        {
            auto conn_id = res->get_uint(id_col);
            auto user = res->get_string(user_col);
            string kill_query = mxs::string_printf("KILL SOFT CONNECTION %li;", conn_id);
            StopWatch timer;
            if (execute_cmd_time_limit(kill_query, time_remaining, &error_msg))
            {
                MXB_WARNING("Killed connection id %lu to '%s' from super-user '%s' to prevent writes.",
                            conn_id, name(), user.c_str());
            }
            else
            {
                error = true;
                PRINT_MXS_JSON_ERROR(error_out, "Could not kill connection %lu from super-user '%s': %s",
                                     conn_id, user.c_str(), error_msg.c_str());
            }
            time_remaining -= timer.split();
        }
    }
    else
    {
        // If query failed because of insufficient rights, don't consider this an error, just print a warning.
        // Perhaps the user doesn't want the monitor doing this.
        if (error_num == ER_DBACCESS_DENIED_ERROR || error_num == ER_TABLEACCESS_DENIED_ERROR
            || error_num == ER_COLUMNACCESS_DENIED_ERROR)
        {
            MXB_WARNING("Insufficient rights to query logged in super-users for server '%s': %s Super-users "
                        "may perform writes during the cluster manipulation operation.",
                        name(), error_msg.c_str());
        }
        else
        {
            error = true;
            PRINT_MXS_JSON_ERROR(error_out, "Could not query connected super-users: %s", error_msg.c_str());
        }
    }
    return !error;
}