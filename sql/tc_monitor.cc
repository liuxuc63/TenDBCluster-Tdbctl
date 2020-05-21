/*
    Copyright (C) 2020 THL A29 Limited, a Tencent company.  All rights reserved.
*/

#include "sql_base.h"         // open_tables, open_and_lock_tables,
#include "sql_lex.h"
#include "sp_head.h"
#include "tc_base.h"
#include "sql_servers.h"
#include "mysql.h"
#include "sql_common.h"
#include "m_string.h"
#include "handler.h"
#include "log.h"
#include <string.h>
#include <iostream>
#include <string>
#include <map>
#include <set>
#include <list>
#include <vector>
#include <sstream>
#include <regex>
#include <thread>
#include <mutex>
#include "tc_monitor.h"

static int64 current_id = 0;
static bool current_id_init = FALSE;
static PSI_memory_key key_memory_monitor;
map<string, MYSQL*> spider_conn_map;
/*
key:ip#port
value:server_name
*/
map<string, string> spider_server_name_map;
map<string, string> spider_user_map;
map<string, string> spider_passwd_map;
set<string> spider_ipport_set;
map<string, string> tdbctl_ipport_map;
map<string, string> tdbctl_user_map;
map<string, string> tdbctl_passwd_map;
string  tdbctl_server_name="";
string  lock_time_sql="";
MYSQL *tdbctl_primary_conn = NULL;
MEM_ROOT mem_root;


void tc_free_connect()
{
	tc_conn_free(spider_conn_map);
	if (tdbctl_primary_conn)
	{
		mysql_close(tdbctl_primary_conn);
		tdbctl_primary_conn = NULL;
	}
	spider_conn_map.clear();
	spider_ipport_set.clear();
	spider_user_map.clear();
	spider_passwd_map.clear();
	spider_server_name_map.clear();
	tdbctl_ipport_map.clear();
	tdbctl_user_map.clear();
	tdbctl_passwd_map.clear();
	free_root(&mem_root, MYF(0));
}

int set_mysql_options(int &error_code, string &message) 
{
	int result = 0;
	stringstream ss;
	tc_exec_info exec_info;
	ulong read_timeout = 600;
	ulong write_timeout = 600;
	ulong connect_timeout = 60;
	read_timeout = read_timeout < tc_check_availability_interval ?
		read_timeout : tc_check_availability_interval;
	write_timeout = write_timeout < tc_check_availability_interval ?
		write_timeout : tc_check_availability_interval;
	connect_timeout = connect_timeout < tc_check_availability_interval ?
		connect_timeout : tc_check_availability_interval;
	lock_time_sql = "set lock_wait_timeout=";
	ss.str("");
	ss << tc_check_availability_interval;
	lock_time_sql += ss.str();
	mysql_options(tdbctl_primary_conn, MYSQL_OPT_READ_TIMEOUT, &read_timeout);
	mysql_options(tdbctl_primary_conn, MYSQL_OPT_WRITE_TIMEOUT, &write_timeout);
	mysql_options(tdbctl_primary_conn, MYSQL_OPT_CONNECT_TIMEOUT, &connect_timeout);
	if (tc_exec_sql_without_result(tdbctl_primary_conn, lock_time_sql, &exec_info))
	{
		result = 2;
		error_code = exec_info.err_code;
		message = exec_info.err_msg;
		exec_info.err_code = 0;
		exec_info.row_affect = 0;
		exec_info.err_msg = "";
		return result;
	}
	map<string, MYSQL*>::iterator its;
	for (its = spider_conn_map.begin(); its != spider_conn_map.end(); its++)
	{
		mysql_options(its->second, MYSQL_OPT_READ_TIMEOUT, &read_timeout);
		mysql_options(its->second, MYSQL_OPT_WRITE_TIMEOUT, &write_timeout);
		mysql_options(its->second, MYSQL_OPT_CONNECT_TIMEOUT, &connect_timeout);
		if (tc_exec_sql_without_result(its->second, lock_time_sql, &exec_info))
		{
			result = 2;
			error_code = exec_info.err_code;
			message = exec_info.err_msg;
			exec_info.err_code = 0;
			exec_info.row_affect = 0;
			exec_info.err_msg = "";
			return result;
		}
	}
	return result;
}

int tc_init_connect(ulong& server_version)
{
	int ret = 0;
	int error_code=0;
	string message="";
	tc_free_connect();

	init_sql_alloc(key_memory_monitor, &mem_root, ACL_ALLOC_BLOCK_SIZE, 0);
	spider_ipport_set = get_spider_ipport_set(
		&mem_root,
		spider_user_map,
		spider_passwd_map,
		FALSE);
	spider_server_name_map = get_server_name_map(&mem_root, SPIDER_WRAPPER, false);
	spider_conn_map = tc_spider_conn_connect(
		ret,
		spider_ipport_set,
		spider_user_map,
		spider_passwd_map);
	if (ret)
	{
		goto finish;
	}
	//get all TDBCTL
	tdbctl_ipport_map = get_tdbctl_ipport_map(
		&mem_root,
		tdbctl_user_map,
		tdbctl_passwd_map);
	tdbctl_primary_conn = tc_tdbctl_conn_primary(
		ret,
		tdbctl_ipport_map,
		tdbctl_user_map,
		tdbctl_passwd_map);
	if (ret) 
	{
		goto finish;
	}
	if((ret=set_mysql_options(error_code, message)))
		goto finish;
	//tdbctl_slave also need to do monitor
	tdbctl_server_name = tc_get_server_name(ret, &mem_root, TDBCTL_WRAPPER, true);
	if (ret)
	{
		goto finish;
	}
	server_version = get_modify_server_version();
	current_id_init = FALSE;
	return ret;
finish:
	tc_free_connect();
	if (error_code) 
	{
		sql_print_warning("TDBCTL MONITOR: tc connect fail: error code is %d, error message: %s",
				error_code, (char*)(message.data()));
	}
	return ret;
}


void create_check_cluster_availability_thread()
{
	std::thread t(tc_check_cluster_availability_thread);
	t.detach();
}


/*
init schema and data for cluster by random spider  
*/
int tc_check_cluster_availability_init()
{
	int result = 0;
	int ret = 0;
	stringstream ss;
	tc_exec_info exec_info;
	MEM_ROOT mem_root;
	map<string, string> spider_user_map;
	map<string, string> spider_passwd_map;
	set<string>  spider_ipport_set;
	MYSQL* spider_single_conn = NULL;
	init_sql_alloc(key_memory_monitor, &mem_root, ACL_ALLOC_BLOCK_SIZE, 0);

	//init for sql
	string sql = "set ddl_execute_by_ctl = on";
	string create_db_sql = "create database if not exists cluster_monitor";
	string drop_table_sql = "drop table if exists cluster_monitor.cluster_heartbeat";
	string create_table_sql = "create table if not exists cluster_monitor.cluster_heartbeat( "
		"uid int(11) NOT NULL, "
		"time timestamp NOT NULL DEFAULT CURRENT_TIMESTAMP ON UPDATE CURRENT_TIMESTAMP, "
		"k int(11) DEFAULT 0, "
		"PRIMARY KEY(uid) "
		") ENGINE = InnoDB ";
	string replace_sql = "replace into   cluster_monitor.cluster_heartbeat(uid) values";
	string init_sql = "";
	string replace_sql_cur = "";
	ulong num = get_servers_count_by_wrapper(MYSQL_WRAPPER, FALSE);
	ulong num_tmp = num;
	vector<string>vec(num,"");
	if (num <= 0)
	{
		goto finish;
	}
	for (ulong i = 0;num_tmp>0;++i) 
	{
		ss.str("");
		ss << i;
		string str = ss.str();
		int hash_value = (int)(((longlong)crc32(0L, (uchar*)(&str)->c_str(),
			(&str)->length()))%num);
		if (vec[hash_value] == "")
		{
			--num_tmp;
			ss.str("");
			ss << i;
			vec[hash_value] = ss.str();
			replace_sql_cur = "(";
			replace_sql_cur += ss.str();
			replace_sql_cur += "),";
			replace_sql += replace_sql_cur;
		}
	}
	replace_sql.erase(replace_sql.end() - 1);
	init_sql = sql + ";" + create_db_sql + ";" + drop_table_sql + ";" +
		create_table_sql + ";" + replace_sql;
	
	spider_ipport_set = get_spider_ipport_set(
		&mem_root,
		spider_user_map,
		spider_passwd_map,
		FALSE);

	spider_single_conn = tc_spider_conn_single(
		ret,
		spider_ipport_set,
		spider_user_map,
		spider_passwd_map);
	if (ret)
	{
		result = 1;
		goto finish;
	}
	if (tc_exec_sql_without_result(spider_single_conn, init_sql, &exec_info))
	{
		result = 2;
		goto finish;
	}
finish:
	mysql_close(spider_single_conn);
	spider_ipport_set.clear();
	spider_user_map.clear();
	spider_passwd_map.clear();
	free_root(&mem_root, MYF(0));
	return result;
}

string tc_generate_id()
{
	string str_id = "";
	stringstream ss;
	if (!current_id_init)
	{
		MYSQL_RES* res;
		string sql_max_id = "select max(id) from cluster_admin.cluster_heartbeat_log";
		MYSQL_ROW row = NULL;
		res = tc_exec_sql_with_result(tdbctl_primary_conn, sql_max_id);
		if (res && (row = mysql_fetch_row(res)))
		{
			current_id = row[0] ? atoi(row[0]) : 0;
			current_id_init = true;
		}
		else
		{
			goto finish;
		}
	}
	current_id = (current_id + 1) % max_heartbeat_log;
	ss.str("");
	ss << current_id;
	str_id = ss.str();
finish:
	return str_id;
}

/*
check_cluster_availability
1.update cluster cluster_monitor.cluster_heartbeat table
2.log result in TDBCTL: cluster_admin.cluster_heartbeat_log
*/
int tc_check_cluster_availability()
{
	int result = 0;
	tc_exec_info exec_info;
	stringstream ss;
	map<string, MYSQL*>::iterator its;

	//init for sql
	string check_heartbeat_sql = "update cluster_monitor.cluster_heartbeat set k=(k+1)%1024";
	string heartbeat_log_sql_pre = "replace into cluster_admin.cluster_heartbeat_log( "
		"id, tdbctl_name, server_name, host, code, message) values(";
	string quotation = "\"";
	string spider_server_name = "";
	string host = "";
	string error_code = "0";
	string message = "";
	string str_id = tc_generate_id();
	if (str_id.size() < 1)
	{
		result = 2;
		goto finish;
	}
	for (its = spider_conn_map.begin(); its != spider_conn_map.end(); its++)
	{	
		host = its->first;
		spider_server_name = spider_server_name_map[its->first];
		MYSQL* spider_conn = its->second;
		if (tc_exec_sql_without_result(spider_conn, check_heartbeat_sql, &exec_info))
		{
			result = 1;
			ss.str("");
			ss << exec_info.err_code;
			error_code = ss.str();
			message = exec_info.err_msg;
			exec_info.err_code = 0;
			exec_info.row_affect = 0;
			exec_info.err_msg = "";
		}
		if (tc_monitor_log(tdbctl_server_name, spider_server_name, host, error_code, message)) 
		{
			result = 2;
		}
	}
finish:
	if (result == 2)
	{
		sql_print_warning("TDBCTL MONITOR: select or replace cluster_heartbeat_log failed");
	}
	return result;
}

/*
result:
0 ok
1 monitor error
2 select or replace cluster_heartbeat_log  error, need to record error_log
*/
int tc_process_monitor_log()
{
	int result = 0;
	bool cluster_monitor_reuslt = true;
	int tdbctl_num = tdbctl_ipport_map.size();
	int num_all;
	// num of TDBCTL which monitor ok
	int num_ok;
	// num of TDBCTL which monitor error
	int num_error;
	//record the time of process result
	ulong t = 0;
	tc_exec_info exec_info;
	MYSQL_RES* res;
	MYSQL_ROW row = NULL;
	map<string, string>::iterator its;
	time_t to_tm_time = (time_t)time((time_t*)0);
	struct tm lt;
	time_t to_tm_time_new;
	struct tm* l_time_new = NULL;
	to_tm_time_new = to_tm_time - tc_check_availability_interval;
	l_time_new = localtime_r(&to_tm_time_new, &lt);
	char time_string[30] = { 0 };
	snprintf(time_string, sizeof(time_string) - 1, "%4d-%02d-%02d %02d:%02d:%02d",
		1900 + l_time_new->tm_year,
		1 + l_time_new->tm_mon,
		l_time_new->tm_mday,
		l_time_new->tm_hour,
		l_time_new->tm_min,
		l_time_new->tm_sec);
	string quotation = "\"";
	string select_sql = "select count(*) from cluster_admin.cluster_heartbeat_log where time>=";
	select_sql += quotation;
	select_sql += time_string;
	select_sql += quotation;
	map<string, string> spider_server_name_map_tmp = spider_server_name_map;
	while (spider_server_name_map_tmp.size() > 0 && t < tc_check_availability_interval)
	{
		for (its = spider_server_name_map_tmp.begin(); its != spider_server_name_map_tmp.end();)
		{
			num_all = 0;
			num_ok = 0;
			num_error = 0;
			/*
			select_num_all = "select count(*) from cluster_admin.cluster_heartbeat_log where
			time>\"2020-04-25 20:11:17\" and server_name=\"SPIDER0\""
			*/
			string select_num_all = select_sql + " and server_name=";
			select_num_all += quotation + its->second + quotation;
			res = tc_exec_sql_with_result(tdbctl_primary_conn, select_num_all);
			if (res && (row = mysql_fetch_row(res)))
			{
				num_all = row[0] ? atoi(row[0]) : 0;		
			}
			if (num_all > 0) 
			{
				/*
				select_num_ok = "select count(*) from cluster_admin.cluster_heartbeat_log where
				time>\"'2020-04-25 20:11:17'\" and server_name=\"SPIDER0\" and code=0"
				*/
				string select_num_ok = select_num_all + " and code=0";
				res = tc_exec_sql_with_result(tdbctl_primary_conn, select_num_ok);
				if (res && (row = mysql_fetch_row(res)))
				{
					num_ok = row[0] ? atoi(row[0]) : 0;
				}
				if (num_ok > 0)
				{
					//log ok for spider node
					if (!(tc_master_monitor_log(true, time_string, its->second)))
					{
						spider_server_name_map_tmp.erase(its++);
						continue;
					}
				}
				else if (num_ok == 0) 
				{
					string select_num_error = select_num_all + " and code>0";
					res = tc_exec_sql_with_result(tdbctl_primary_conn, select_num_error);
					if (res && (row = mysql_fetch_row(res)))
					{
						num_error = row[0] ? atoi(row[0]) : 0;
					}
					if (num_error == tdbctl_num) 
					{
						//log error for spider node
						cluster_monitor_reuslt = false;
						if (!(tc_master_monitor_log(false, time_string, its->second)))
						{
							spider_server_name_map_tmp.erase(its++);
							continue;
						}
					}
				}
			}
			++its;
		}
		if (spider_server_name_map_tmp.size())
		{
			sleep(1);
			++t;
		}
	}
	/*
	if  size>0
	means some spider node not all TDBCTL vote error.
	if there is and record, then re-use it 
	else log with unknown
	*/
	if (spider_server_name_map_tmp.size() > 0)
	{
		cluster_monitor_reuslt = false;
		for (its = spider_server_name_map_tmp.begin(); its != spider_server_name_map_tmp.end(); ++its)
		{
			string select_num_all = select_sql + " and server_name= ";
			select_num_all += quotation + its->second + quotation;

			string select_num_error = select_num_all + " and code>0 ";
			res = tc_exec_sql_with_result(tdbctl_primary_conn, select_num_error);
			if (res && (row = mysql_fetch_row(res)))
			{
				num_error = row[0] ? atoi(row[0]) : 0;
			}
			else
			{
				result = 2;
				continue;
			}
			//if there is and record, then re-use it
			if (num_error)
			{
				if (tc_master_monitor_log(false, time_string, its->second))
				{
					result = 2;
					continue;
				}
			}
			else/*log with unknown*/
			{
				if (tc_monitor_log("",its->second, its->first, "1", "unknown, can't get result"))
				{
					result = 2;
					continue;
				}
			}
		}
	}

	if (cluster_monitor_reuslt) 
	{
	   //log ok for cluster
		if (tc_monitor_log("", CLUSTER_FLAG, "", "0", ""))
		{
			result = 2;
		}
		tc_is_available = 1;
	}
	else
	{
		//log error for cluster
		if (tc_monitor_log("", CLUSTER_FLAG, "", "1", "error"))
		{
			result = 2;
		}
		tc_is_available = 0;
	}
	if (result == 2)
	{
		sql_print_warning("TDBCTL MONITOR: select or replace cluster_heartbeat_log failed");
	}
	return result;
}

/*
log in cluster_admin.cluster_heartbeat_log by master_tdbctl
flag
0 for error
1 for ok
*/
int tc_master_monitor_log(bool flag, string time_string, string spider_server_name)
{
	int result = 0;
	tc_exec_info exec_info;
	string quotation = "\"";
	string replace_sql = "replace into cluster_admin.cluster_heartbeat_log "
		" (id,server_name,host,code,message) select ";
	string str_id = tc_generate_id();
	if (str_id.size() < 1)
	{
		result = 2;
		goto finish;
	}
	replace_sql += str_id;
	replace_sql += " as id,server_name,host,code,message from "
		" cluster_admin.cluster_heartbeat_log where time>";
	replace_sql += quotation;
	replace_sql += time_string;
	replace_sql += quotation;
	replace_sql += " and server_name=";
	replace_sql += quotation;
	replace_sql += spider_server_name;
	replace_sql += quotation;
	if (flag)
	{
		replace_sql += " and code=0 limit 1";
	}
	else 
	{
		replace_sql += " and code>0 limit 1";
	}
	/*
	replace into cluster_admin.cluster_heartbeat_log (id,server_name,host,code,message)
	select 33840 as id,server_name,host,code,message from
	cluster_admin.cluster_heartbeat_log where time>'2020-04-25 20:22:28'
	and server_name="SPIDER0" and code>0 limit 1
	*/
	if (tc_exec_sql_without_result(tdbctl_primary_conn, replace_sql, &exec_info))
	{
		result = 3;
		sql_print_warning("TDBCTL MONITOR: log in cluster_heartbeat_log failed :%02d %s", 
			exec_info.err_code, (char*)(exec_info.err_msg.data()));
		exec_info.err_code = 0;
		exec_info.row_affect = 0;
		exec_info.err_msg = "";
	}
finish:
	return result;
}

int tc_monitor_log(string tdbctl_name, string spider_server_name, string host,
	string error_code, string message) 
{
	int result = 0;
	tc_exec_info exec_info;
	string quotation = "\"";
	string heartbeat_log_sql = "replace into cluster_admin.cluster_heartbeat_log( "
		"id, tdbctl_name, server_name, host, code, message) values(";
	string str_id = tc_generate_id();
	if (str_id.size() < 1)
	{
		result = 2;
		goto finish;
	}
	//append sql
	heartbeat_log_sql += str_id;
	heartbeat_log_sql += ",";
	heartbeat_log_sql += quotation;
	heartbeat_log_sql += tdbctl_name;
	heartbeat_log_sql += quotation;
	heartbeat_log_sql += ",";
	heartbeat_log_sql += quotation;
	heartbeat_log_sql += spider_server_name;
	heartbeat_log_sql += quotation;
	heartbeat_log_sql += ",";
	heartbeat_log_sql += quotation;
	heartbeat_log_sql += host;
	heartbeat_log_sql += quotation;
	heartbeat_log_sql += ",";
	heartbeat_log_sql += error_code;
	heartbeat_log_sql += ",";
	heartbeat_log_sql += quotation;
	heartbeat_log_sql += message;
	heartbeat_log_sql += quotation;
	heartbeat_log_sql += ")";
	/*
	heartbeat_log_sql = "replace into cluster_admin.cluster_heartbeat_log
	( id, tdbctl_name, server_name, host, code, message)
	values(33839,\"TDBCTL0\",\"SPIDER0\",\"127.0.0.1#5000\",0,\"\")"
	*/
	if (tc_exec_sql_without_result(tdbctl_primary_conn, heartbeat_log_sql, &exec_info))
	{
		result = 3;
		sql_print_warning("TDBCTL MONITOR: log in cluster_heartbeat_log failed :%02d %s",
			exec_info.err_code, (char*)(exec_info.err_msg.data()));
		exec_info.err_code = 0;
		exec_info.row_affect = 0;
		exec_info.err_msg = "";
	}
finish:
	return result;
}

int do_servers_reload()
{
	int ret = 0;
	THD  *thd = new THD();
	if (thd == NULL)
	{
		ret = 1;
		goto finish;
	}
	my_thread_init();
	thd->thread_stack = (char*)&thd;
	thd->store_globals();
	if (servers_reload(thd))
	{
		ret = 1;
		my_error(ER_TCADMIN_EXECUTE_ERROR, MYF(0), "reload server failed");
		goto finish;
	}
finish:
	delete thd;
	return ret;
}
/*
check cluster availability
tc_check_cluster_availability do check work and log in cluster_admin.cluster_heartbeat_log
*/
void tc_check_cluster_availability_thread()
{
	/*
	flag of whether need to re-init connect
	0 means not re-init
	1 means re-init

	after re-init ok , set flag=0
	if the TDBCTL is not primary or user set global tc_check_availability=0 ,
	then flag=1 and  free connect
	*/
	int flag = 1;

	/*
	result of  tc_init_connect  and tc_check_cluster_availability
	0 means ok
	1 means error
	if error,need to re-init connection
	*/
	int res = 0;
	ulong server_version = -1;

  while (1)
  {
		/*
      if tc_check_availability=1 and is primary TDBCTL
      TODO:get tc_tdbctl_conn_primary by host and port
    */
    if (tc_check_availability)
    {
      /*
        if current node is not primary, do servers_reload to get
        latest mysql.server
      */
      if (tc_is_primary_tdbctl_node() != 1)
      {
        if (do_servers_reload())
        {
          sleep(tc_check_availability_interval);
          continue;
        }
      }
      /*
        if first time do check
        or do  tc_init_connect  and tc_check_cluster_availability error
        or mysql.servers cache changes
      */
      if (flag || res ||  check_server_version(server_version))
      {
        //init memory and connect
        if (!(res = tc_init_connect(server_version)))
          flag = 0;
        else
        {
          /*
            fail to init memory and connect
            if the mysql.servers cache is empty, init connect is always failed,
            so it is need to reload when node is slave
          */
          sleep(tc_check_availability_interval);
          tc_is_available = 0;
          continue;
        }
      }

      /*
        if init memory and connect ok
        then start monitor
      */
      if (!res)
      {
        //check available for cluster
        res = tc_check_cluster_availability();
        if ((tc_is_primary_tdbctl_node() == 1) &&
              tc_process_monitor_log())
          res = 1;

        for (ulong i = 0; i < labs(tc_check_availability_interval - 2); ++i)
          sleep(1);
      }
    }
    else
    {
      if (!flag)
      {
        //free memory and connect
        tc_free_connect();
        flag = 1;
      }
      tc_is_available = 0;
    }
    sleep(2);
  }
}
