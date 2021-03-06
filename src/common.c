/**
* Copyright (C) 2008 Happy Fish / YuQing
*
* FastDFS may be copied only under the terms of the GNU General
* Public License V3, which may be found in the FastDFS source kit.
* Please visit the FastDFS Home Page http://www.csource.org/ for more detail.
**/


#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <limits.h>
#include <time.h>
#include <unistd.h>
#include "fdfs_define.h"
#include "logger.h"
#include "shared_func.h"
#include "fdfs_global.h"
#include "sockopt.h"
#include "http_func.h"
#include "fdfs_http_shared.h"
#include "fdfs_client.h"
#include "local_ip_func.h"
#include "fdfs_shared_func.h"
#include "trunk_shared.h"
#include "common.h"

#define FDFS_MOD_REPONSE_MODE_PROXY	'P'
#define FDFS_MOD_REPONSE_MODE_REDIRECT	'R'

typedef struct tagGroupStorePaths {
	char group_name[FDFS_GROUP_NAME_MAX_LEN + 1];
	int group_name_len;
	int storage_server_port;
	FDFSStorePaths store_paths;
} GroupStorePaths;

static int storage_server_port = FDFS_STORAGE_SERVER_DEF_PORT;
static int my_group_name_len = 0;
static int group_count = 0;  //for multi groups
static bool url_have_group_name = false;
static bool use_storage_id = false;
static bool flv_support = false;  //if support flv
static char flv_extension[FDFS_FILE_EXT_NAME_MAX_LEN + 1] = {0};  //flv extension name
static int  flv_ext_len = 0;  //flv extension length
static char my_group_name[FDFS_GROUP_NAME_MAX_LEN + 1] = {0};
static char response_mode = FDFS_MOD_REPONSE_MODE_PROXY;
static GroupStorePaths *group_store_paths = NULL;   //for multi groups
static FDFSHTTPParams g_http_params;
static int storage_sync_file_max_delay = 24 * 3600;

static int fdfs_get_params_from_tracker();
static int fdfs_format_http_datetime(time_t t, char *buff, const int buff_size);

static int fdfs_strtoll(const char *s, int64_t *value)
{
	char *end = NULL;
	*value = strtoll(s, &end, 10);
	if (end != NULL && *end != '\0')
	{
		return EINVAL;
	}

	return 0;
}

static int fdfs_load_groups_store_paths(IniContext *pItemContext)
{
	char section_name[64];
	char *pGroupName;
	int bytes;
	int result;
	int i;

	bytes = sizeof(GroupStorePaths) * group_count;
	group_store_paths = (GroupStorePaths *)malloc(bytes);
	if (group_store_paths == NULL)
	{
		logError("file: "__FILE__", line: %d, " \
			"malloc %d bytes fail, " \
			"errno: %d, error info: %s", \
			__LINE__, bytes, errno, STRERROR(errno));
		return errno != 0 ? errno : ENOMEM;
	}

	for (i=0; i<group_count; i++)
	{
		sprintf(section_name, "group%d", i + 1);
		pGroupName = iniGetStrValue(section_name, "group_name", \
				pItemContext);
		if (pGroupName == NULL)
		{
			logError("file: "__FILE__", line: %d, " \
				"section: %s, you must set parameter: " \
				"group_name!", __LINE__, section_name);
			return ENOENT;
		}

		group_store_paths[i].storage_server_port = iniGetIntValue( \
			section_name, "storage_server_port", pItemContext, \
			FDFS_STORAGE_SERVER_DEF_PORT);

		group_store_paths[i].group_name_len = snprintf( \
			group_store_paths[i].group_name, \
			sizeof(group_store_paths[i].group_name), \
			"%s", pGroupName);
		if (group_store_paths[i].group_name_len == 0)
		{
			logError("file: "__FILE__", line: %d, " \
				"section: %s, parameter: group_name " \
				"can't be empty!", __LINE__, section_name);
			return EINVAL;
		}
		
		group_store_paths[i].store_paths.paths = \
			storage_load_paths_from_conf_file_ex(pItemContext, \
			section_name, false, &group_store_paths[i].store_paths.count, \
			&result);
		if (result != 0)
		{
			return result;
		}
	}

	return 0;
}

int fdfs_mod_init()
{
        IniContext iniContext;
	int result;
	int len;
	int i;
	char *pLogFilename;
	char *pReponseMode;
	char *pIfAliasPrefix;
	char buff[2 * 1024];
	bool load_fdfs_parameters_from_tracker = false;

	log_init();
	trunk_shared_init();

	if ((result=iniLoadFromFile(FDFS_MOD_CONF_FILENAME, &iniContext)) != 0)
	{
		logError("file: "__FILE__", line: %d, " \
			"load conf file \"%s\" fail, ret code: %d", \
			__LINE__, FDFS_MOD_CONF_FILENAME, result);
		return result;
	}

	do
	{
	group_count = iniGetIntValue(NULL, "group_count", &iniContext, 0);
	if (group_count < 0)
	{
		logError("file: "__FILE__", line: %d, " \
			"conf file: %s, group_count: %d is invalid!", \
			__LINE__, FDFS_MOD_CONF_FILENAME, group_count);
		return EINVAL;
	}

	url_have_group_name = iniGetBoolValue(NULL, "url_have_group_name", \
						&iniContext, false);
	if (group_count > 0)
	{
		if (!url_have_group_name)
		{
			logError("file: "__FILE__", line: %d, "   \
				"config file: %s, you must set "  \
				"url_have_group_name to true to " \
				"support multi-group!", \
				__LINE__, FDFS_MOD_CONF_FILENAME);
			result = ENOENT;
			break;
		}

		if ((result=fdfs_load_groups_store_paths(&iniContext)) != 0)
		{
			break;
		}
	}
	else
	{
		char *pGroupName;

		pGroupName = iniGetStrValue(NULL, "group_name", &iniContext);
		if (pGroupName == NULL)
		{
			logError("file: "__FILE__", line: %d, " \
				"config file: %s, you must set parameter: " \
				"group_name!", __LINE__, FDFS_MOD_CONF_FILENAME);
			result = ENOENT;
			break;
		}

		my_group_name_len = snprintf(my_group_name, \
				sizeof(my_group_name), "%s", pGroupName);
		if (my_group_name_len == 0)
		{
			logError("file: "__FILE__", line: %d, " \
				"config file: %s, parameter: group_name " \
				"can't be empty!", __LINE__, \
				FDFS_MOD_CONF_FILENAME);
			result = EINVAL;
			break;
		}

		if ((result=storage_load_paths_from_conf_file(&iniContext)) != 0)
		{
			break;
		}
	}

	g_fdfs_connect_timeout = iniGetIntValue(NULL, "connect_timeout", \
			&iniContext, DEFAULT_CONNECT_TIMEOUT);
	if (g_fdfs_connect_timeout <= 0)
	{
		g_fdfs_connect_timeout = DEFAULT_CONNECT_TIMEOUT;
	}

	g_fdfs_network_timeout = iniGetIntValue(NULL, "network_timeout", \
			&iniContext, DEFAULT_NETWORK_TIMEOUT);
	if (g_fdfs_network_timeout <= 0)
	{
		g_fdfs_network_timeout = DEFAULT_NETWORK_TIMEOUT;
	}

	load_log_level(&iniContext);

	pLogFilename = iniGetStrValue(NULL, "log_filename", &iniContext);
	if (pLogFilename != NULL && *pLogFilename != '\0')
	{
		if ((result=log_set_filename(pLogFilename)) != 0)
		{
			break;
		}
	}

	storage_server_port = iniGetIntValue(NULL, "storage_server_port", \
			&iniContext, FDFS_STORAGE_SERVER_DEF_PORT);

	if ((result=fdfs_http_params_load(&iniContext, FDFS_MOD_CONF_FILENAME, \
		&g_http_params)) != 0)
	{
		break;
	}

	pReponseMode = iniGetStrValue(NULL, "response_mode", &iniContext);
	if (pReponseMode != NULL)
	{
		if (strcmp(pReponseMode, "redirect") == 0)
		{
			response_mode = FDFS_MOD_REPONSE_MODE_REDIRECT;
		}
	}

	pIfAliasPrefix = iniGetStrValue (NULL, "if_alias_prefix", &iniContext);
	if (pIfAliasPrefix == NULL)
	{
		*g_if_alias_prefix = '\0';
	}
	else
	{
		snprintf(g_if_alias_prefix, sizeof(g_if_alias_prefix),
			"%s", pIfAliasPrefix);
	}

	load_fdfs_parameters_from_tracker = iniGetBoolValue(NULL, \
				"load_fdfs_parameters_from_tracker", \
				&iniContext, false);
	if (load_fdfs_parameters_from_tracker)
	{
		result = fdfs_load_tracker_group_ex(&g_tracker_group, \
				FDFS_MOD_CONF_FILENAME, &iniContext);
	}
	else
	{
		storage_sync_file_max_delay = iniGetIntValue(NULL, \
				"storage_sync_file_max_delay", \
                	        &iniContext, 24 * 3600);
		use_storage_id = iniGetBoolValue(NULL, "use_storage_id", \
				&iniContext, false);
		if (use_storage_id)
		{
			result = fdfs_load_storage_ids_from_file( \
					FDFS_MOD_CONF_FILENAME, &iniContext);
		}
	}

	} while (false);

	flv_support = iniGetBoolValue(NULL, "flv_support", \
					&iniContext, false);
	if (flv_support)
	{
		char *flvExtension;
		flvExtension = iniGetStrValue (NULL, "flv_extension", \
					&iniContext);
		if (flvExtension == NULL)
		{
			flv_ext_len = sprintf(flv_extension, "flv");
		}
		else
		{
			flv_ext_len = snprintf(flv_extension, \
				sizeof(flv_extension), "%s", flvExtension);
		}
	}

	iniFreeContext(&iniContext);
	if (result != 0)
	{
		return result;
	}

	load_local_host_ip_addrs();
	if (load_fdfs_parameters_from_tracker)
	{
		fdfs_get_params_from_tracker();
	}

	if (group_count > 0)
	{
		len = sprintf(buff, "group_count=%d, ", group_count);
	}
	else
	{
		len = sprintf(buff, "group_name=%s, storage_server_port=%d, " \
			"path_count=%d, ", my_group_name, \
			storage_server_port, g_fdfs_store_paths.count);
		for (i=0; i<g_fdfs_store_paths.count; i++)
		{
			len += snprintf(buff + len, sizeof(buff) - len, \
				"store_path%d=%s, ", i, \
				g_fdfs_store_paths.paths[i]);
		}
	}

	logInfo("fastdfs apache / nginx module v1.15, " \
		"response_mode=%s, " \
		"base_path=%s, " \
		"url_have_group_name=%d, " \
		"%s" \
		"connect_timeout=%d, "\
		"network_timeout=%d, "\
		"tracker_server_count=%d, " \
		"if_alias_prefix=%s, " \
		"local_host_ip_count=%d, " \
		"need_find_content_type=%d, " \
		"default_content_type=%s, " \
		"anti_steal_token=%d, " \
		"token_ttl=%ds, " \
		"anti_steal_secret_key length=%d, "  \
		"token_check_fail content_type=%s, " \
		"token_check_fail buff length=%d, "  \
		"load_fdfs_parameters_from_tracker=%d, " \
		"storage_sync_file_max_delay=%ds, " \
		"use_storage_id=%d, storage server id count=%d, " \
		"flv_support=%d, flv_extension=%s", \
		response_mode == FDFS_MOD_REPONSE_MODE_PROXY ? \
			"proxy" : "redirect", \
		g_fdfs_base_path, url_have_group_name, buff, \
		g_fdfs_connect_timeout, g_fdfs_network_timeout, \
		g_tracker_group.server_count, \
		g_if_alias_prefix, g_local_host_ip_count, \
		g_http_params.need_find_content_type, \
		g_http_params.default_content_type, \
		g_http_params.anti_steal_token, \
		g_http_params.token_ttl, \
		g_http_params.anti_steal_secret_key.length, \
		g_http_params.token_check_fail_content_type, \
		g_http_params.token_check_fail_buff.length, \
		load_fdfs_parameters_from_tracker, \
		storage_sync_file_max_delay, use_storage_id, \
		g_storage_id_count, flv_support, flv_extension);

	if (group_count > 0)
	{
		int k;
		for (k=0; k<group_count; k++)
		{
			len = 0;
			*buff = '\0';
			for (i=0; i<group_store_paths[k].store_paths.count; i++)
			{
				len += snprintf(buff + len, sizeof(buff) - len, \
					", store_path%d=%s", i, \
					group_store_paths[k].store_paths.paths[i]);
			}

			logInfo("group %d. group_name=%s, " \
				"storage_server_port=%d, path_count=%d%s", \
				k + 1, group_store_paths[k].group_name, \
				storage_server_port, group_store_paths[k]. \
				store_paths.count, buff);
		}
	}
	//print_local_host_ip_addrs();

	return 0;
}

#define OUTPUT_HEADERS(pContext, pResponse, http_status) \
	pResponse->status = http_status;  \
	pContext->output_headers(pContext->arg, pResponse);

static int fdfs_download_callback(void *arg, const int64_t file_size, \
		const char *data, const int current_size)
{
	struct fdfs_download_callback_args *pCallbackArgs;

	pCallbackArgs = (struct fdfs_download_callback_args *)arg;

	if (!pCallbackArgs->pResponse->header_outputed)
	{
		pCallbackArgs->pResponse->content_length = file_size;
		OUTPUT_HEADERS(pCallbackArgs->pContext, \
			pCallbackArgs->pResponse, HTTP_OK)
	}

	pCallbackArgs->sent_bytes += current_size;
	return pCallbackArgs->pContext->send_reply_chunk( \
		pCallbackArgs->pContext->arg, \
		(pCallbackArgs->sent_bytes == file_size) ? 1 : 0, \
		data, current_size);
}

static void fdfs_format_range(const struct fdfs_http_range *range, \
	struct fdfs_http_response *pResponse)
{
	if (range->start < 0)
	{
		pResponse->range_len = sprintf(pResponse->range, \
			"bytes="INT64_PRINTF_FORMAT, range->start);
	}
	else if (range->end == 0)
	{
		pResponse->range_len = sprintf(pResponse->range, \
			"bytes="INT64_PRINTF_FORMAT"-", range->start);
	}
	else
	{
		pResponse->range_len = sprintf(pResponse->range, \
			"bytes="INT64_PRINTF_FORMAT"-"INT64_PRINTF_FORMAT, \
			range->start, range->end);
	}
}

static void fdfs_format_content_range(const struct fdfs_http_range *range, \
	const int64_t file_size, struct fdfs_http_response *pResponse)
{
	pResponse->content_range_len = sprintf(pResponse->content_range, \
		"bytes "INT64_PRINTF_FORMAT"-"INT64_PRINTF_FORMAT \
		"/"INT64_PRINTF_FORMAT, range->start, range->end, file_size);
}

static int fdfs_check_and_format_range(struct fdfs_http_range *range, 
	const int64_t file_size)
{
	if (range->start < 0)
	{
		int64_t start;
		start = range->start + file_size;
		if (start < 0)
		{
			logError("file: "__FILE__", line: %d, " \
				"invalid range value: "INT64_PRINTF_FORMAT, \
				__LINE__, range->start);
			return EINVAL;
		}
		range->start = start;
	}
	else if (range->start >= file_size)
	{
		logError("file: "__FILE__", line: %d, " \
			"invalid range start value: "INT64_PRINTF_FORMAT \
			", exceeds file size: "INT64_PRINTF_FORMAT, \
			__LINE__, range->start, file_size);
		return EINVAL;
	}

	if (range->end == 0)
	{
		range->end = file_size - 1;
	}
	else if (range->end >= file_size)
	{
		logError("file: "__FILE__", line: %d, " \
			"invalid range end value: "INT64_PRINTF_FORMAT \
			", exceeds file size: "INT64_PRINTF_FORMAT, \
			__LINE__, range->end, file_size);
		return EINVAL;
	}

	if (range->start > range->end)
	{
		logError("file: "__FILE__", line: %d, " \
			"invalid range value, start: "INT64_PRINTF_FORMAT \
			", exceeds end: "INT64_PRINTF_FORMAT, \
			__LINE__, range->start, range->end);
		return EINVAL;
	}

	return 0;
}

#define FDFS_SET_LAST_MODIFIED(response, pContext, mtime) \
	do { \
		response.last_modified = mtime; \
		fdfs_format_http_datetime(response.last_modified, \
			response.last_modified_buff, \
			sizeof(response.last_modified_buff)); \
		if (*pContext->if_modified_since != '\0') \
		{ \
			if (strcmp(response.last_modified_buff, \
				pContext->if_modified_since) == 0) \
			{ \
			OUTPUT_HEADERS(pContext, (&response), HTTP_NOTMODIFIED)\
			return HTTP_NOTMODIFIED; \
			} \
		} \
		\
		/*\
		logInfo("last_modified: %s, if_modified_since: %s, strcmp=%d", \
			response.last_modified_buff, \
			pContext->if_modified_since, \
			strcmp(response.last_modified_buff, \
			pContext->if_modified_since)); \
		*/ \
	} while (0)

int fdfs_http_request_handler(struct fdfs_http_context *pContext)
{
#define HTTPD_MAX_PARAMS   32
	char *file_id_without_group;
	char *url;
	char file_id[128];
	char uri[256];
	int url_len;
	int uri_len;
	int param_count;
	int ext_len;
	KeyValuePair params[HTTPD_MAX_PARAMS];
	char *p;
	char *filename;
	const char *ext_name;
	FDFSStorePaths *pStorePaths;
	char true_filename[128];
	char full_filename[MAX_PATH_SIZE + 64];
	char content_type[64];
	char file_trunk_buff[FDFS_OUTPUT_CHUNK_SIZE];
	struct stat file_stat;
	int64_t file_offset;
	int64_t file_size;
	int64_t download_bytes;
	off_t remain_bytes;
	int read_bytes;
	int filename_len;
	int full_filename_len;
	int store_path_index;
	int fd;
	int result;
	int http_status;
	int the_storage_port;
	struct fdfs_http_response response;
	FDFSFileInfo file_info;
	bool bFileExists;
	bool bSameGroup;  //if in my group
	bool bTrunkFile;
	FDFSTrunkFullInfo trunkInfo;

	memset(&response, 0, sizeof(response));
	response.status = HTTP_OK;

	//logInfo("url=%s", pContext->url);

	url_len = strlen(pContext->url);
	if (url_len < 16)
	{
		logError("file: "__FILE__", line: %d, " \
			"url length: %d < 16", __LINE__, url_len);
		OUTPUT_HEADERS(pContext, (&response), HTTP_BADREQUEST)
		return HTTP_BADREQUEST;
	}

	if (strncasecmp(pContext->url, "http://", 7) == 0)
	{
		p = strchr(pContext->url + 7, '/');
		if (p == NULL)
		{
			logError("file: "__FILE__", line: %d, " \
				"invalid url: %s", __LINE__, pContext->url);
			OUTPUT_HEADERS(pContext, (&response), HTTP_BADREQUEST)
			return HTTP_BADREQUEST;
		}

		uri_len = url_len - (p - pContext->url);
		url = p;
	}
	else
	{
		uri_len = url_len;
		url = pContext->url;
	}

	if (uri_len + 1 >= (int)sizeof(uri))
	{
		logError("file: "__FILE__", line: %d, " \
			"uri length: %d is too long, >= %d", __LINE__, \
			uri_len, (int)sizeof(uri));
		OUTPUT_HEADERS(pContext, (&response), HTTP_BADREQUEST)
		return HTTP_BADREQUEST;
	}

	if (*url != '/')
	{
		*uri = '/';
		memcpy(uri+1, url, uri_len+1);
		uri_len++;
	}
	else
	{
		memcpy(uri, url, uri_len+1);
	}

	the_storage_port = storage_server_port;
	param_count = http_parse_query(uri, params, HTTPD_MAX_PARAMS);
	if (url_have_group_name)
	{
		int group_name_len;

		snprintf(file_id, sizeof(file_id), "%s", uri + 1);
		file_id_without_group = strchr(file_id, '/');
		if (file_id_without_group == NULL)
		{
			logError("file: "__FILE__", line: %d, " \
				"no group name in url, uri: %s", __LINE__, uri);
			OUTPUT_HEADERS(pContext, (&response), HTTP_BADREQUEST)
			return HTTP_BADREQUEST;
		}

		pStorePaths = &g_fdfs_store_paths;
		group_name_len = file_id_without_group - file_id;
		if (group_count == 0)
		{
			bSameGroup = (group_name_len == my_group_name_len) && \
					(memcmp(file_id, my_group_name, \
						group_name_len) == 0);
		}
		else
		{
			int i;

			bSameGroup = false;
			for (i=0; i<group_count; i++)
			{
			if (group_store_paths[i].group_name_len == \
				group_name_len && memcmp(file_id, \
					group_store_paths[i].group_name, \
					group_name_len) == 0)
			{
				the_storage_port = group_store_paths[i]. \
						storage_server_port;
				bSameGroup = true;
				pStorePaths = &group_store_paths[i].store_paths;
				break;
			}
			}
		}

		file_id_without_group++;  //skip /
	}
	else
	{
		pStorePaths = &g_fdfs_store_paths;
		bSameGroup = true;
		file_id_without_group = uri + 1; //skip /
		snprintf(file_id, sizeof(file_id), "%s/%s", \
			my_group_name, file_id_without_group);
	}

	if (strlen(file_id_without_group) < 22)
	{
		logError("file: "__FILE__", line: %d, " \
			"file id is too short, length: %d < 22, " \
			"uri: %s", __LINE__, \
			(int)strlen(file_id_without_group), uri);
		OUTPUT_HEADERS(pContext, (&response), HTTP_BADREQUEST)
		return HTTP_BADREQUEST;
	}

	if (g_http_params.anti_steal_token)
	{
		char *token;
		char *ts;
		int timestamp;

		token = fdfs_http_get_parameter("token", params, param_count);
		ts = fdfs_http_get_parameter("ts", params, param_count);
		if (token == NULL || ts == NULL)
		{
			logError("file: "__FILE__", line: %d, " \
				"expect parameter token or ts in url, " \
				"uri: %s", __LINE__, uri);
			OUTPUT_HEADERS(pContext, (&response), HTTP_BADREQUEST)
			return HTTP_BADREQUEST;
		}

		timestamp = atoi(ts);
		if ((result=fdfs_http_check_token( \
				&g_http_params.anti_steal_secret_key, \
				file_id_without_group, timestamp, token, \
				g_http_params.token_ttl)) != 0)
		{
			logError("file: "__FILE__", line: %d, " \
				"check token fail, uri: %s, " \
				"errno: %d, error info: %s", \
				__LINE__, uri, result, STRERROR(result));
			if (*(g_http_params.token_check_fail_content_type))
			{
				response.content_length = g_http_params. \
						token_check_fail_buff.length;
				response.content_type = g_http_params. \
						token_check_fail_content_type;
				OUTPUT_HEADERS(pContext, (&response), HTTP_OK)

				pContext->send_reply_chunk(pContext->arg, 1, \
					g_http_params.token_check_fail_buff.buff, 
					g_http_params.token_check_fail_buff.length);

				return HTTP_OK;
			}
			else
			{
				OUTPUT_HEADERS(pContext, (&response), HTTP_BADREQUEST)
				return HTTP_BADREQUEST;
			}
		}
	}

	filename = file_id_without_group;
	filename_len = strlen(filename);

	//logInfo("filename=%s", filename);
	if (storage_split_filename_no_check(filename, \
		&filename_len, true_filename, &store_path_index) != 0)
	{
		OUTPUT_HEADERS(pContext, (&response), HTTP_BADREQUEST)
		return HTTP_BADREQUEST;
	}
	if (bSameGroup)
	{
		if (store_path_index < 0 || \
			store_path_index >= pStorePaths->count)
		{
			logError("file: "__FILE__", line: %d, " \
				"filename: %s is invalid, " \
				"invalid store path index: %d, " \
				"which < 0 or >= %d", __LINE__, filename, \
				store_path_index, pStorePaths->count);

			OUTPUT_HEADERS(pContext, (&response), HTTP_BADREQUEST)
			return HTTP_BADREQUEST;
		}
	}

	if (fdfs_check_data_filename(true_filename, filename_len) != 0)
	{
		OUTPUT_HEADERS(pContext, (&response), HTTP_BADREQUEST)
		return HTTP_BADREQUEST;
	}

	if ((result=fdfs_get_file_info_ex1(file_id, false, &file_info)) != 0)
	{
		if (result == ENOENT)
		{
			http_status = HTTP_NOTFOUND;
		}
		else
		{
			http_status = HTTP_INTERNAL_SERVER_ERROR;
		}

		OUTPUT_HEADERS(pContext, (&response), http_status)
		return http_status;
	}
	
	if (file_info.file_size >= 0)  //mormal file
	{
		FDFS_SET_LAST_MODIFIED(response, pContext, \
				file_info.create_timestamp);
	}

	fd = -1;
	memset(&file_stat, 0, sizeof(file_stat));
	if (bSameGroup)
	{
        	FDFSTrunkHeader trunkHeader;
		if ((result=trunk_file_stat_ex1(pStorePaths, store_path_index, \
			true_filename, filename_len, &file_stat, \
			&trunkInfo, &trunkHeader, &fd)) != 0)
		{
			bFileExists = false;
		}
		else
		{
			bFileExists = true;
		}
	}
	else
	{
		bFileExists = false;
		memset(&trunkInfo, 0, sizeof(trunkInfo));
	}

	response.attachment_filename = fdfs_http_get_parameter("filename", \
						params, param_count);
	if (bFileExists)
	{
		if (file_info.file_size < 0)  //slave or appender file
		{
			FDFS_SET_LAST_MODIFIED(response, pContext, \
					file_stat.st_mtime);
		}
	}
	else
	{
		char *redirect;

		//logInfo("source id: %d", file_info.source_id);
		//logInfo("source ip addr: %s", file_info.source_ip_addr);
		//logInfo("create_timestamp: %d", file_info.create_timestamp);

		if (bSameGroup && (is_local_host_ip(file_info.source_ip_addr) \
			|| (file_info.create_timestamp > 0 && (time(NULL) - \
			file_info.create_timestamp > storage_sync_file_max_delay))))
		{
			if (IS_TRUNK_FILE_BY_ID(trunkInfo))
			{
				if (result == ENOENT)
				{
					logError("file: "__FILE__", line: %d, "\
						"logic file: %s not exist", \
						__LINE__, filename);
				}
				else
				{
					logError("file: "__FILE__", line: %d, "\
						"stat logic file: %s fail, " \
						"errno: %d, error info: %s", \
						__LINE__, filename, result, \
						STRERROR(result));
				}
			}
			else
			{
				snprintf(full_filename, \
					sizeof(full_filename), "%s/data/%s", \
					pStorePaths->paths[store_path_index], \
					true_filename);
				if (result == ENOENT)
				{
					logError("file: "__FILE__", line: %d, "\
						"file: %s not exist", \
						__LINE__, full_filename);
				}
				else
				{
					logError("file: "__FILE__", line: %d, "\
						"stat file: %s fail, " \
						"errno: %d, error info: %s", \
						__LINE__, full_filename, \
						result, STRERROR(result));
				}
			}

			OUTPUT_HEADERS(pContext, (&response), HTTP_NOTFOUND)
			return HTTP_NOTFOUND;
		}

		redirect = fdfs_http_get_parameter("redirect", \
						params, param_count);
		if (redirect != NULL)
		{
			logWarning("file: "__FILE__", line: %d, " \
				"redirect again, url: %s", \
				__LINE__, url);

			OUTPUT_HEADERS(pContext, (&response), HTTP_BADREQUEST)
			return HTTP_BADREQUEST;
		}

		if (*(file_info.source_ip_addr) == '\0')
		{
			logWarning("file: "__FILE__", line: %d, " \
				"can't get ip address of source storage " \
				"id: %d, url: %s", __LINE__, \
				file_info.source_id, url);

			OUTPUT_HEADERS(pContext, (&response), \
				HTTP_INTERNAL_SERVER_ERROR)
			return HTTP_INTERNAL_SERVER_ERROR;
		}

		if (response_mode == FDFS_MOD_REPONSE_MODE_REDIRECT)
		{
			char *path_split_str;
			char port_part[16];
			char param_split_char;
			
			if (pContext->server_port == 80)
			{
				*port_part = '\0';
			}
			else
			{
				sprintf(port_part, ":%d", pContext->server_port);
			}

			if (param_count == 0)
			{
				param_split_char = '?';
			}
			else
			{
				param_split_char = '&';
			}

			if (*url != '/')
			{
				path_split_str = "/";
			}
			else
			{
				path_split_str = "";
			}
	
			response.redirect_url_len = snprintf( \
				response.redirect_url, \
				sizeof(response.redirect_url), \
				"http://%s%s%s%s%c%s", \
				file_info.source_ip_addr, port_part, \
				path_split_str, url, \
				param_split_char, "redirect=1");

			logDebug("file: "__FILE__", line: %d, " \
				"redirect to %s", \
				__LINE__, response.redirect_url);

			if (pContext->if_range)
			{
				fdfs_format_range(&(pContext->range), &response);
			}
			OUTPUT_HEADERS(pContext, (&response), HTTP_MOVETEMP)
			return HTTP_MOVETEMP;
		}
		else if (pContext->proxy_handler != NULL)
		{
			return pContext->proxy_handler(pContext->arg, \
					file_info.source_ip_addr);
		}
	}

	ext_name = fdfs_http_get_file_extension(true_filename, \
			filename_len, &ext_len);
	if (g_http_params.need_find_content_type)
	{
	if (fdfs_http_get_content_type_by_extname(&g_http_params, \
		ext_name, ext_len, content_type, sizeof(content_type)) != 0)
	{
		if (fd >= 0)
		{
			close(fd);
		}
		OUTPUT_HEADERS(pContext, (&response), HTTP_SERVUNAVAIL)
		return HTTP_SERVUNAVAIL;
	}
	response.content_type = content_type;
	}

	if (bFileExists)
	{
		file_size = file_stat.st_size;
	}
	else
	{
		bool if_get_file_info;
		if_get_file_info = pContext->header_only || \
				(pContext->if_range && file_info.file_size < 0);
		if (if_get_file_info)
		{
			if ((result=fdfs_get_file_info_ex1(file_id, true, \
				&file_info)) != 0)
			{
				if (result == ENOENT)
				{
					http_status = HTTP_NOTFOUND;
				}
				else
				{
					http_status = HTTP_INTERNAL_SERVER_ERROR;
				}

				OUTPUT_HEADERS(pContext, (&response), http_status)
				return http_status;
			}
		}

		file_size = file_info.file_size;
	}

	if (pContext->if_range)
	{
		if (fdfs_check_and_format_range(&(pContext->range), \
			file_size) != 0)
		{
			if (fd >= 0)
			{
				close(fd);
			}

			OUTPUT_HEADERS(pContext, (&response), HTTP_BADREQUEST)
			return HTTP_BADREQUEST;
		}

		download_bytes = (pContext->range.end - pContext->range.start) + 1;
		fdfs_format_content_range(&(pContext->range), \
					file_size, &response);
	}
	else
	{
		download_bytes = file_size > 0 ? file_size : 0;

		//flv support
		if (flv_support && (flv_ext_len == ext_len && \
			memcmp(ext_name, flv_extension, ext_len) == 0))
		{
			char *pStart;
			pStart = fdfs_http_get_parameter("start", \
						params, param_count);
			if (pStart != NULL)
			{
				int64_t start;
				if (fdfs_strtoll(pStart, &start) == 0)
				{
				if (start >= 0 && (start < file_size \
					|| file_size < 0))
				{
					pContext->range.start = start;
					if (file_size > 0)
					{
					download_bytes = file_size - start;
					}
				}
				}
			}
		}
	}

	if (pContext->header_only)
	{
		if (fd >= 0)
		{
			close(fd);
		}
		response.content_length = download_bytes;
		OUTPUT_HEADERS(pContext, (&response), pContext->if_range ? \
			HTTP_PARTIAL_CONTENT : HTTP_OK )

		return HTTP_OK;
	}

	if (!bFileExists)
	{
		ConnectionInfo storage_server;
		struct fdfs_download_callback_args callback_args;
		int64_t file_size;

		strcpy(storage_server.ip_addr, file_info.source_ip_addr);
		storage_server.port = the_storage_port;
		storage_server.sock = -1;

		callback_args.pContext = pContext;
		callback_args.pResponse = &response;
		callback_args.sent_bytes = 0;

		result = storage_download_file_ex1(NULL, \
                	&storage_server, file_id, \
                	pContext->range.start, download_bytes, \
			fdfs_download_callback, &callback_args, &file_size);

		logDebug("file: "__FILE__", line: %d, " \
			"storage_download_file_ex1 return code: %d, " \
			"file id: %s", __LINE__, result, file_id);

		if (result == 0)
		{
			http_status = HTTP_OK;
		}
		if (result == ENOENT)
		{
			http_status = HTTP_NOTFOUND;
		}
		else
		{
			http_status = HTTP_INTERNAL_SERVER_ERROR;
		}

		OUTPUT_HEADERS(pContext, (&response), http_status)
		return http_status;
	}

	bTrunkFile = IS_TRUNK_FILE_BY_ID(trunkInfo);
	if (bTrunkFile)
	{
		trunk_get_full_filename_ex(pStorePaths, &trunkInfo, \
				full_filename, sizeof(full_filename));
		full_filename_len = strlen(full_filename);
		file_offset = TRUNK_FILE_START_OFFSET(trunkInfo) + \
				pContext->range.start;
	}
	else
	{
		full_filename_len = snprintf(full_filename, \
				sizeof(full_filename), "%s/data/%s", \
				pStorePaths->paths[store_path_index], \
				true_filename);
		file_offset = pContext->range.start;
	}

	response.content_length = download_bytes;
	if (pContext->send_file != NULL && !bTrunkFile)
	{
		http_status = pContext->if_range ? \
				HTTP_PARTIAL_CONTENT : HTTP_OK;
		OUTPUT_HEADERS(pContext, (&response), http_status)
		return pContext->send_file(pContext->arg, full_filename, \
				full_filename_len, file_offset, download_bytes);
	}

	if (fd < 0)
	{
		fd = open(full_filename, O_RDONLY);
		if (fd < 0)
		{
			logError("file: "__FILE__", line: %d, " \
				"open file %s fail, " \
				"errno: %d, error info: %s", __LINE__, \
				full_filename, errno, STRERROR(errno));
				OUTPUT_HEADERS(pContext, (&response), \
						HTTP_SERVUNAVAIL)
			return HTTP_SERVUNAVAIL;
		}
		if (file_offset > 0 && lseek(fd, file_offset, SEEK_SET) < 0)
		{
			close(fd);
			logError("file: "__FILE__", line: %d, " \
				"lseek file: %s fail, " \
				"errno: %d, error info: %s", \
				__LINE__, full_filename, \
				errno, STRERROR(errno));
			OUTPUT_HEADERS(pContext, (&response), \
					HTTP_INTERNAL_SERVER_ERROR)
			return HTTP_INTERNAL_SERVER_ERROR;
		}
	}
	else
	{
		if (pContext->range.start > 0 && \
			lseek(fd, pContext->range.start, SEEK_CUR) < 0)
		{
			close(fd);
			logError("file: "__FILE__", line: %d, " \
				"lseek file: %s fail, " \
				"errno: %d, error info: %s", \
				__LINE__, full_filename, \
				errno, STRERROR(errno));
			OUTPUT_HEADERS(pContext, (&response), \
					HTTP_INTERNAL_SERVER_ERROR)
			return HTTP_INTERNAL_SERVER_ERROR;
		}
	}

	OUTPUT_HEADERS(pContext, (&response), pContext->if_range ? \
                        HTTP_PARTIAL_CONTENT : HTTP_OK)

	remain_bytes = download_bytes;
	while (remain_bytes > 0)
	{
		read_bytes = remain_bytes <= FDFS_OUTPUT_CHUNK_SIZE ? \
			     remain_bytes : FDFS_OUTPUT_CHUNK_SIZE;
		if (read(fd, file_trunk_buff, read_bytes) != read_bytes)
		{
			close(fd);
			logError("file: "__FILE__", line: %d, " \
				"read from file %s fail, " \
				"errno: %d, error info: %s", __LINE__, \
				full_filename, errno, STRERROR(errno));
			return HTTP_INTERNAL_SERVER_ERROR;
		}

		remain_bytes -= read_bytes;
		if (pContext->send_reply_chunk(pContext->arg, \
			(remain_bytes == 0) ? 1: 0, file_trunk_buff, \
			read_bytes) != 0)
		{
			close(fd);
			return HTTP_INTERNAL_SERVER_ERROR;
		}
	}

	close(fd);
	return HTTP_OK;
}

static int fdfs_get_params_from_tracker()
{
        IniContext iniContext;
	int result;
	bool continue_flag;

	continue_flag = false;
	if ((result=fdfs_get_ini_context_from_tracker(&g_tracker_group, \
		&iniContext, &continue_flag, false, NULL)) != 0)
        {
                return result;
        }

	storage_sync_file_max_delay = iniGetIntValue(NULL, \
                        "storage_sync_file_max_delay", \
                        &iniContext, 24 * 3600);

	use_storage_id = iniGetBoolValue(NULL, "use_storage_id", \
				&iniContext, false);
        iniFreeContext(&iniContext);

	if (use_storage_id)
	{
		result = fdfs_get_storage_ids_from_tracker_group( \
				&g_tracker_group);
	}

        return result;
}

static int fdfs_format_http_datetime(time_t t, char *buff, const int buff_size)
{
	struct tm tm;
	struct tm *ptm;

	*buff = '\0';
	if ((ptm=gmtime_r(&t, &tm)) == NULL)
	{
		return errno != 0 ? errno : EFAULT;
	}

	strftime(buff, buff_size, "%a, %d %b %Y %H:%M:%S GMT", ptm);
	return 0;
}

int fdfs_parse_range(const char *value, struct fdfs_http_range *range)
{
/*
range format:
bytes=500-999
bytes=-500
bytes=9500-
*/
#define RANGE_PREFIX_STR  "bytes="
#define RANGE_PREFIX_LEN   (int)(sizeof(RANGE_PREFIX_STR) - 1)

	int len;
	int result;
	const char *p;
	const char *pEndPos;
	char buff[32];

	len = strlen(value);
	if (len <= RANGE_PREFIX_LEN + 1)
	{
		return EINVAL;
	}

	p = value + RANGE_PREFIX_LEN;
	if (*p == '-')
	{
		if ((result=fdfs_strtoll(p, &(range->start))) != 0)
		{
			return result;
		}
		range->end = 0;
		return 0;
	}

	pEndPos = strchr(p, '-');
	if (pEndPos == NULL)
	{
		return EINVAL;
	}

	len = pEndPos - p;
	if (len >= (int)sizeof(buff))
	{
		return EINVAL;
	}
	memcpy(buff, p, len);
	*(buff + len) = '\0';
	if ((result=fdfs_strtoll(buff, &(range->start))) != 0)
	{
		return result;
	}

	pEndPos++; //skip -
	if (*pEndPos == '\0')
	{
		range->end = 0;
	}
	else
	{
		if ((result=fdfs_strtoll(pEndPos, &(range->end))) != 0)
		{
			return result;
		}
	}

	return 0;
}

