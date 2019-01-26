/*
 * Copyright (c) 2019 MariaDB Corporation Ab
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
#pragma once

#include "config.hh"
#include "sql.hh"
#include "processor.hh"

/**
 * A class that handles the execution of SQL statements on a server.
 *
 * The actual execution of the SQL is done on a separate thread and the SQL statements are only enqueued for
 * execution by the main controlling thread.
 */
class SQLExecutor : public REProc
{
public:
    SQLExecutor(const cdc::Server& servers);

protected:
    bool process(const std::vector<MARIADB_RPL_EVENT*>& queue) override;
    bool start_transaction() override;
    bool commit_transaction() override;
    void rollback_transaction() override;

private:
    bool connect();
    bool process_query(MARIADB_RPL_EVENT* event);
    bool process_uservar(MARIADB_RPL_EVENT* event);

    std::unique_ptr<SQL> m_sql;         // The current database connection
    cdc::Server          m_server;      // The ordered list of servers where the SQLExecutor connects to
};
