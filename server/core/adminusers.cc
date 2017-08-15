/*
 * Copyright (c) 2016 MariaDB Corporation Ab
 *
 * Use of this software is governed by the Business Source License included
 * in the LICENSE.TXT file and at www.mariadb.com/bsl11.
 *
 * Change Date: 2020-01-01
 *
 * On the date above, in accordance with the Business Source License, use
 * of this software will be governed by version 2 or later of the General
 * Public License.
 */
#include <maxscale/cdefs.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <unistd.h>
#include <crypt.h>
#include <sys/stat.h>
#include <string>

#include <maxscale/users.h>
#include <maxscale/adminusers.h>
#include <maxscale/log_manager.h>
#include <maxscale/paths.h>
#include <maxscale/json_api.h>

/**
 * @file adminusers.c - Administration user account management
 */

static USERS *load_linux_users();
static USERS *load_inet_users();

static const char *admin_add_user(USERS** pusers, const char* fname,
                                  const char* uname, const char* password);
static const char* admin_remove_user(USERS *users, const char* fname, const char *uname);



static USERS *linux_users = NULL;
static USERS *inet_users = NULL;

static const int LINELEN = 80;

static const char LINUX_USERS_FILE_NAME[] = "maxadmin-users";
static const char INET_USERS_FILE_NAME[]  = "passwd";

/**
 * Admin Users initialisation
 */
void admin_users_init()
{
    linux_users = load_linux_users();
    inet_users = load_inet_users();
}

static bool admin_dump_users(USERS* users, const char* fname)
{
    char path[PATH_MAX];

    if (access(get_datadir(), F_OK) != 0)
    {
        if (mkdir(get_datadir(), S_IRWXU) != 0 && errno != EEXIST)
        {
            MXS_ERROR("Failed to create directory '%s': %d, %s",
                      get_datadir(), errno, mxs_strerror(errno));
            return false;
        }
    }

    snprintf(path, sizeof(path), "%s/%s", get_datadir(), fname);
    json_t* json = users_to_json(users);
    bool rval = true;

    if (json_dump_file(json, path, 0) == -1)
    {
        MXS_ERROR("Failed to dump admin users to file");
        rval = false;
    }

    return rval;
}

static const char *admin_add_user(USERS** pusers, const char* fname,
                                  const char* uname, const char* password)
{
    if (*pusers == NULL)
    {
        *pusers = users_alloc();
    }

    if (!users_add(*pusers, uname, password ? password : "", ACCOUNT_ADMIN))
    {
        return ADMIN_ERR_DUPLICATE;
    }

    if (!admin_dump_users(*pusers, fname))
    {
        return ADMIN_ERR_FILEOPEN;
    }

    return ADMIN_SUCCESS;
}

static const char* admin_remove_user(USERS *users, const char* fname, const char *uname)
{
    if (strcmp(uname, DEFAULT_ADMIN_USER) == 0)
    {
        MXS_WARNING("Attempt to delete the default admin user '%s'.", uname);
        return ADMIN_ERR_DELROOT;
    }

    if (!users_delete(users, uname))
    {
        MXS_ERROR("Couldn't find user %s. Removing user failed.", uname);
        return ADMIN_ERR_USERNOTFOUND;
    }

    if (!admin_dump_users(users, fname))
    {
        return ADMIN_ERR_FILEOPEN;
    }

    return ADMIN_SUCCESS;
}

static json_t* admin_user_json_data(const char* host, const char* user, enum user_type user_type)
{
    ss_dassert(user_type != USER_TYPE_ALL);
    const char* type = user_type == USER_TYPE_INET ? CN_INET : CN_UNIX;

    json_t* entry = json_object();
    json_object_set_new(entry, CN_ID, json_string(user));
    json_object_set_new(entry, CN_TYPE, json_string(type));

    std::string self = MXS_JSON_API_USERS;
    self += type;
    json_object_set_new(entry, CN_RELATIONSHIPS, mxs_json_self_link(host, self.c_str(), user));

    return entry;
}

static void user_types_to_json(USERS* users, json_t* arr, const char* host, enum user_type type)
{
    json_t* json = users_diagnostic_json(users);
    size_t index;
    json_t* value;

    json_array_foreach(json, index, value)
    {
        json_array_append_new(arr, admin_user_json_data(host, json_string_value(value), type));
    }

    json_decref(json);
}

static std::string path_from_type(enum user_type type)
{
    std::string path = MXS_JSON_API_USERS;

    if (type == USER_TYPE_INET)
    {
        path += CN_INET;
    }
    else if (type == USER_TYPE_UNIX)
    {
        path += CN_UNIX;
    }

    return path;
}

json_t* admin_user_to_json(const char* host, const char* user, enum user_type type)
{
    std::string path = path_from_type(type);
    path += "/";
    path += user;

    return mxs_json_resource(host, path.c_str(), admin_user_json_data(host, user, type));
}

json_t* admin_all_users_to_json(const char* host, enum user_type type)
{
    json_t* arr = json_array();
    std::string path = path_from_type(type);

    if (inet_users && (type == USER_TYPE_ALL || type == USER_TYPE_INET))
    {
        user_types_to_json(inet_users, arr, host, USER_TYPE_INET);
    }

    if (linux_users && (type == USER_TYPE_ALL || type == USER_TYPE_UNIX))
    {
        user_types_to_json(linux_users, arr, host, USER_TYPE_UNIX);
    }

    return mxs_json_resource(host, path.c_str(), arr);
}

USERS* load_legacy_users(FILE* fp)
{
    USERS *rval;
    char  path[PATH_MAX];
    char  uname[80];
    int added_users = 0;

    if ((rval = users_alloc()) == NULL)
    {
        return NULL;
    }
    while (fgets(uname, sizeof(uname), fp))
    {
        char *nl = strchr(uname, '\n');

        if (nl)
        {
            *nl = '\0';
        }
        else if (!feof(fp))
        {
            MXS_ERROR("Line length exceeds %d characters, possibly corrupted "
                      "'passwd' file in: %s", LINELEN, path);
            users_free(rval);
            rval = NULL;
            break;
        }

        const char *password;
        char *colon = strchr(uname, ':');
        if (colon)
        {
            // Inet case
            *colon = 0;
            password = colon + 1;
        }
        else
        {
            // Linux case.
            password = "";
        }

        if (users_add(rval, uname, password, ACCOUNT_ADMIN))
        {
            added_users++;
        }
    }

    if (!added_users)
    {
        users_free(rval);
        rval = NULL;
    }

    return rval;
}

/**
 * Load the admin users
 *
 * @param fname Name of the file in the datadir to load
 *
 * @return Table of users
 */
static USERS* load_users(const char *fname)
{
    USERS *rval = NULL;
    char path[PATH_MAX];
    snprintf(path, sizeof(path), "%s/%s", get_datadir(), fname);
    FILE* fp = fopen(path, "r");

    if (fp)
    {
        json_error_t err;
        json_t* json = json_loadf(fp, 0, &err);

        if (json)
        {
            /** New format users */
            rval = users_from_json(json);
        }
        else
        {
            /** Old style users file */
            rval = load_legacy_users(fp);

            if (rval)
            {
                /** Users loaded successfully, back up the original file and
                 * replace it with the new one */
                const char backup_suffix[] = ".backup";
                char newpath[strlen(path) + sizeof(backup_suffix)];
                sprintf(newpath, "%s%s", path, backup_suffix);

                if (rename(path, newpath) != 0)
                {
                    MXS_ERROR("Failed to rename old users file: %d, %s",
                              errno, mxs_strerror(errno));
                }
                else if (!admin_dump_users(rval, fname))
                {
                    MXS_ERROR("Failed to dump new users. Please rename the file "
                              "'%s' manually to '%s' and restart MaxScale to "
                              "attempt again.", newpath, path);
                }
                else
                {
                    MXS_NOTICE("Upgraded users file at '%s' to new format, "
                               "backup of the old file is stored in '%s'.",
                               newpath, path);
                }
            }
        }

        fclose(fp);
    }

    return rval;
}


static USERS *load_linux_users()
{
    return load_users(LINUX_USERS_FILE_NAME);
}

static USERS *load_inet_users()
{
    return load_users(INET_USERS_FILE_NAME);
}

/**
 * Enable Linux account
 *
 * @param uname Name of Linux user
 *
 * @return NULL on success or an error string on failure.
 */
const char *admin_enable_linux_account(const char *uname)
{
    return admin_add_user(&linux_users, LINUX_USERS_FILE_NAME, uname, NULL);
}

/**
 * Disable Linux account
 *
 * @param uname Name of Linux user
 *
 * @return NULL on success or an error string on failure.
 */
const char* admin_disable_linux_account(const char* uname)
{
    return admin_remove_user(linux_users, LINUX_USERS_FILE_NAME, uname);
}

/**
 * Check whether Linux account is enabled
 *
 * @param uname The user name
 *
 * @return True if the account is enabled, false otherwise.
 */
bool admin_linux_account_enabled(const char *uname)
{
    bool rv = false;

    if (strcmp(uname, DEFAULT_ADMIN_USER) == 0)
    {
        rv = true;
    }
    else if (linux_users)
    {
        rv = users_find(linux_users, uname);
    }

    return rv;
}

#define MXS_CRYPT_SIZE 60

void mxs_crypt(const char* password, const char* salt, char* output)
{
#if HAVE_GLIBC
    struct crypt_data cdata;
    cdata.initialized = 0;
    char* pw = crypt_r(password, salt, &cdata);
    snprintf(output, MXS_CRYPT_SIZE, "%s", pw);
#else
    static SPINLOCK mxs_crypt_lock = SPINLOCK_INIT;
    spinlock_acquire(&mxs_crypt_lock);
    char* pw = crypt(password, salt);
    snprintf(output, MXS_CRYPT_SIZE, "%s", pw);
    spinlock_release(&mxs_crypt_lock);
#endif
}

/**
 * Add insecure remote (network) user.
 *
 * @param uname    Name of the new user.
 * @param password Password of the new user.
 *
 * @return NULL on success or an error string on failure.
 */
const char *admin_add_inet_user(const char *uname, const char* password)
{
    char cpassword[MXS_CRYPT_SIZE];
    mxs_crypt(password, ADMIN_SALT, cpassword);

    return admin_add_user(&inet_users, INET_USERS_FILE_NAME, uname, cpassword);
}

/**
 * Remove insecure remote (network) user
 *
 * @param uname    Name of user to be removed.
 * @param password Password of user to be removed.
 *
 * @return NULL on success or an error string on failure.
 */
const char* admin_remove_inet_user(const char* uname)
{
    return admin_remove_user(inet_users, INET_USERS_FILE_NAME, uname);
}

/**
 * Check for existance of remote user.
 *
 * @param user The user name to test.
 *
 * @return True if the user exists, false otherwise.
 */
bool admin_inet_user_exists(const char *uname)
{
    bool rv = false;

    if (inet_users)
    {
        rv = users_find(inet_users, uname);
    }

    return rv;
}

/**
 * Verify a remote user name and password
 *
 * @param username  Username to verify
 * @param password  Password to verify
 *
 * @return True if the username/password combination is valid
 */
bool
admin_verify_inet_user(const char *username, const char *password)
{
    bool rv = false;

    if (inet_users)
    {
        char cpassword[MXS_CRYPT_SIZE];
        mxs_crypt(password, ADMIN_SALT, cpassword);
        rv = users_auth(inet_users, username, cpassword);
    }
    else
    {
        if (strcmp(username, INET_DEFAULT_USERNAME) == 0
            && strcmp(password, INET_DEFAULT_PASSWORD) == 0)
        {
            rv = true;
        }
    }

    return rv;
}

bool admin_is_admin_user(const char* username)
{
    bool rval = true; // The default `admin:mariadb` user has all permissions

    if (inet_users)
    {
        rval = users_is_admin(inet_users, username);
    }

    return rval;
}

/**
 * Print Linux and and inet users
 *
 * @param dcb A DCB to send the output to.
 */
void dcb_PrintAdminUsers(DCB *dcb)
{
    dcb_printf(dcb, "Enabled Linux accounts (secure)    : ");

    if (linux_users)
    {
        users_diagnostic(dcb, linux_users);
    }
    else
    {
        dcb_printf(dcb, "\n");
    }

    dcb_printf(dcb, "Created network accounts (insecure): ");

    if (inet_users)
    {
        users_diagnostic(dcb, inet_users);
    }
    else
    {
        dcb_printf(dcb, "\n");
    }
}
