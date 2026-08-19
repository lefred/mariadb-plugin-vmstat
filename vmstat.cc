/* Copyright (c) 2019,2024,2025,2026 MariaDB Corporation
   Copyright (c) 2026 lefred (Frédéric Descamps)

   This program is free software; you can redistribute it and/or modify
   it under the terms of the GNU General Public License as published by
   the Free Software Foundation; version 2 of the License.

   This program is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
   GNU General Public License for more details.

   You should have received a copy of the GNU General Public License
   along with this program; if not, write to the Free Software
   Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA 02110-1335  USA */

#define MYSQL_SERVER

#include <my_global.h>
#include <mysql/plugin.h>
#include <sql_acl.h>
#include <sql_class.h>
#include <sql_i_s.h>

#include <inttypes.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

namespace {

static const size_t LINE_SIZE= 4096;

static bool may_read_vmstat(THD *thd)
{
  /*
    Host-wide resource usage is operational information.  PROCESS is the
    closest MariaDB privilege to the MySQL component's READ_VMSTAT dynamic
    privilege.  Pass true to avoid replacing the statement error with one
    produced for every attempted table fill.
  */
  return check_global_access(thd, PROCESS_ACL, true) == 0;
}

static ulonglong saturating_multiply(ulonglong value, ulonglong multiplier)
{
  return value > ULONGLONG_MAX / multiplier ? ULONGLONG_MAX
                                             : value * multiplier;
}

static ulonglong nonnegative_difference(ulonglong total, ulonglong values)
{
  return total > values ? total - values : 0;
}

static ulonglong saturating_add(ulonglong left, ulonglong right)
{
  return left > ULONGLONG_MAX - right ? ULONGLONG_MAX : left + right;
}

static void store_unsigned(Field *field, ulonglong value)
{
  field->set_notnull();
  field->store(value, true);
}

namespace Vmstat {

static ST_FIELD_INFO cpu_fields[]=
{
  Show::Column("CPU",           Show::Varchar(32),     NOT_NULL),
  Show::Column("USER_MS",       Show::ULonglong(),     NOT_NULL),
  Show::Column("NICE_MS",       Show::ULonglong(),     NOT_NULL),
  Show::Column("SYSTEM_MS",     Show::ULonglong(),     NOT_NULL),
  Show::Column("IDLE_MS",       Show::ULonglong(),     NOT_NULL),
  Show::Column("IOWAIT_MS",     Show::ULonglong(),     NOT_NULL),
  Show::Column("IRQ_MS",        Show::ULonglong(),     NOT_NULL),
  Show::Column("SOFTIRQ_MS",    Show::ULonglong(),     NOT_NULL),
  Show::Column("STEAL_MS",      Show::ULonglong(),     NOT_NULL),
  Show::Column("GUEST_MS",      Show::ULonglong(),     NOT_NULL),
  Show::Column("GUEST_NICE_MS", Show::ULonglong(),     NOT_NULL),
  Show::Column("PROCS_RUNNING", Show::ULonglong(),     NULLABLE),
  Show::Column("PROCS_BLOCKED", Show::ULonglong(),     NULLABLE),
  Show::CEnd()
};

static int cpu_fill(THD *thd, TABLE_LIST *tables, COND *)
{
  if (!may_read_vmstat(thd))
    return 0;

  FILE *file= fopen("/proc/stat", "r");
  if (!file)
    return 0;

  TABLE *table= tables->table;
  char line[LINE_SIZE];
  long ticks= sysconf(_SC_CLK_TCK);
  const ulonglong ticks_per_second= ticks > 0 ? static_cast<ulonglong>(ticks)
                                              : 100;
  uint64_t running= 0, blocked= 0;
  bool have_running= false, have_blocked= false;
  int error= 0;

  /* Process counters occur after CPU counters, so collect them first. */
  while (fgets(line, sizeof(line), file))
  {
    if (sscanf(line, "procs_running %" SCNu64, &running) == 1)
      have_running= true;
    else if (sscanf(line, "procs_blocked %" SCNu64, &blocked) == 1)
      have_blocked= true;
  }
  rewind(file);

  while (fgets(line, sizeof(line), file))
  {
    char name[33]= {0};
    uint64_t values[10]= {0};
    int count= sscanf(line,
      "%32s %" SCNu64 " %" SCNu64 " %" SCNu64 " %" SCNu64
      " %" SCNu64 " %" SCNu64 " %" SCNu64 " %" SCNu64
      " %" SCNu64 " %" SCNu64,
      name, &values[0], &values[1], &values[2], &values[3], &values[4],
      &values[5], &values[6], &values[7], &values[8], &values[9]);

    if (count >= 5 &&
        strncmp(name, "cpu", 3) == 0 &&
        (name[3] == '\0' || (name[3] >= '0' && name[3] <= '9')) &&
        strspn(name + 3, "0123456789") == strlen(name + 3))
    {
      restore_record(table, s->default_values);
      table->field[0]->store(name, strlen(name), system_charset_info);
      for (uint index= 0; index < 10; ++index)
        store_unsigned(table->field[index + 1],
                       saturating_multiply(values[index], 1000) /
                       ticks_per_second);

      if (name[3] == '\0')
      {
        if (have_running) store_unsigned(table->field[11], running);
        else table->field[11]->set_null();
        if (have_blocked) store_unsigned(table->field[12], blocked);
        else table->field[12]->set_null();
      }
      else
      {
        table->field[11]->set_null();
        table->field[12]->set_null();
      }
      if (schema_table_store_record(thd, table))
      {
        error= 1;
        break;
      }
    }
  }
  fclose(file);
  return error;
}

static ST_FIELD_INFO memory_fields[]=
{
  Show::Column("MEM_TOT",       Show::ULonglong(), NOT_NULL),
  Show::Column("MEM_FREE",      Show::ULonglong(), NOT_NULL),
  Show::Column("MEM_USED",      Show::ULonglong(), NOT_NULL),
  Show::Column("MEM_AVAILABLE", Show::ULonglong(), NOT_NULL),
  Show::Column("TOT_SWAP",      Show::ULonglong(), NOT_NULL),
  Show::Column("SWAP_FREE",     Show::ULonglong(), NOT_NULL),
  Show::Column("SWAP_USED",     Show::ULonglong(), NOT_NULL),
  Show::Column("BUFFERS",       Show::ULonglong(), NOT_NULL),
  Show::Column("CACHED",        Show::ULonglong(), NOT_NULL),
  Show::Column("SWAP_IN",       Show::ULonglong(), NULLABLE),
  Show::Column("SWAP_OUT",      Show::ULonglong(), NULLABLE),
  Show::CEnd()
};

static int memory_fill(THD *thd, TABLE_LIST *tables, COND *)
{
  if (!may_read_vmstat(thd))
    return 0;

  FILE *file= fopen("/proc/meminfo", "r");
  if (!file)
    return 0;

  ulonglong total= 0, free_memory= 0, available= 0, buffers= 0;
  ulonglong cached= 0, slab= 0, sreclaimable= 0, swap_total= 0, swap_free= 0;
  bool have_sreclaimable= false;
  char line[LINE_SIZE], key[64], unit[16];
  uint64_t value;
  while (fgets(line, sizeof(line), file))
  {
    unit[0]= '\0';
    if (sscanf(line, "%63[^:]: %" SCNu64 " %15s", key, &value, unit) < 2)
      continue;
    const ulonglong bytes= strcmp(unit, "kB") == 0
                              ? saturating_multiply(value, 1024) : value;
    if (strcmp(key, "MemTotal") == 0) total= bytes;
    else if (strcmp(key, "MemFree") == 0) free_memory= bytes;
    else if (strcmp(key, "MemAvailable") == 0) available= bytes;
    else if (strcmp(key, "Buffers") == 0) buffers= bytes;
    else if (strcmp(key, "Cached") == 0) cached= bytes;
    else if (strcmp(key, "Slab") == 0) slab= bytes;
    else if (strcmp(key, "SReclaimable") == 0)
    { sreclaimable= bytes; have_sreclaimable= true; }
    else if (strcmp(key, "SwapTotal") == 0) swap_total= bytes;
    else if (strcmp(key, "SwapFree") == 0) swap_free= bytes;
  }
  fclose(file);

  uint64_t page_in= 0, page_out= 0;
  bool have_page_in= false, have_page_out= false;
  file= fopen("/proc/vmstat", "r");
  if (file)
  {
    while (fgets(line, sizeof(line), file))
    {
      if (sscanf(line, "pgpgin %" SCNu64, &page_in) == 1)
        have_page_in= true;
      else if (sscanf(line, "pgpgout %" SCNu64, &page_out) == 1)
        have_page_out= true;
    }
    fclose(file);
  }

  TABLE *table= tables->table;
  restore_record(table, s->default_values);
  /*
    Slab includes SUnreclaim, which the kernel cannot free on demand;
    only SReclaimable counts as reclaimable cache.  Fall back to the
    full Slab figure on kernels old enough to lack SReclaimable.
  */
  const ulonglong reclaimable_slab= have_sreclaimable ? sreclaimable : slab;
  const ulonglong reclaimed= saturating_add(cached, reclaimable_slab);
  const ulonglong unavailable= saturating_add(free_memory, buffers);
  const ulonglong unavailable_all= saturating_add(unavailable, reclaimed);
  const ulonglong row[]= {
    total, free_memory, nonnegative_difference(total, unavailable_all),
    available, swap_total, swap_free,
    nonnegative_difference(swap_total, swap_free), buffers, reclaimed
  };
  for (uint index= 0; index < array_elements(row); ++index)
    store_unsigned(table->field[index], row[index]);
  if (have_page_in) store_unsigned(table->field[9], page_in);
  else table->field[9]->set_null();
  if (have_page_out) store_unsigned(table->field[10], page_out);
  else table->field[10]->set_null();
  return schema_table_store_record(thd, table) ? 1 : 0;
}

static ST_FIELD_INFO disk_fields[]=
{
  Show::Column("DISK",          Show::Varchar(128), NOT_NULL),
  Show::Column("READS",         Show::ULonglong(),  NOT_NULL),
  Show::Column("READ_BYTES",    Show::ULonglong(),  NOT_NULL),
  Show::Column("READ_TIME_MS",  Show::ULonglong(),  NOT_NULL),
  Show::Column("WRITES",        Show::ULonglong(),  NOT_NULL),
  Show::Column("WRITE_BYTES",   Show::ULonglong(),  NOT_NULL),
  Show::Column("WRITE_TIME_MS", Show::ULonglong(),  NOT_NULL),
  Show::Column("FLUSHES",       Show::ULonglong(),  NOT_NULL),
  Show::Column("FLUSH_TIME_MS", Show::ULonglong(),  NOT_NULL),
  Show::CEnd()
};

static int disk_fill(THD *thd, TABLE_LIST *tables, COND *)
{
  if (!may_read_vmstat(thd))
    return 0;

  FILE *file= fopen("/proc/diskstats", "r");
  if (!file)
    return 0;

  TABLE *table= tables->table;
  char line[LINE_SIZE], disk[129];
  int error= 0;
  while (fgets(line, sizeof(line), file))
  {
    uint64_t major, minor, reads, reads_merged, read_sectors, read_ms;
    uint64_t writes, writes_merged, write_sectors, write_ms;
    uint64_t in_flight, io_ms, weighted_ms, discards, discards_merged;
    uint64_t discard_sectors, discard_ms, flushes= 0, flush_ms= 0;
    int count= sscanf(line,
      "%" SCNu64 " %" SCNu64 " %128s "
      "%" SCNu64 " %" SCNu64 " %" SCNu64 " %" SCNu64 " "
      "%" SCNu64 " %" SCNu64 " %" SCNu64 " %" SCNu64 " "
      "%" SCNu64 " %" SCNu64 " %" SCNu64 " "
      "%" SCNu64 " %" SCNu64 " %" SCNu64 " %" SCNu64 " "
      "%" SCNu64 " %" SCNu64,
      &major, &minor, disk, &reads, &reads_merged, &read_sectors, &read_ms,
      &writes, &writes_merged, &write_sectors, &write_ms, &in_flight,
      &io_ms, &weighted_ms, &discards, &discards_merged, &discard_sectors,
      &discard_ms, &flushes, &flush_ms);
    if (count < 14)
      continue;

    restore_record(table, s->default_values);
    table->field[0]->store(disk, strlen(disk), system_charset_info);
    const ulonglong row[]= {
      reads, saturating_multiply(read_sectors, 512), read_ms, writes,
      saturating_multiply(write_sectors, 512), write_ms,
      count >= 20 ? flushes : 0, count >= 20 ? flush_ms : 0
    };
    for (uint index= 0; index < array_elements(row); ++index)
      store_unsigned(table->field[index + 1], row[index]);
    if (schema_table_store_record(thd, table))
    {
      error= 1;
      break;
    }
  }
  fclose(file);
  return error;
}

static int cpu_init(void *ptr)
{
  ST_SCHEMA_TABLE *schema= static_cast<ST_SCHEMA_TABLE *>(ptr);
  schema->fields_info= cpu_fields;
  schema->fill_table= cpu_fill;
  return 0;
}

static int memory_init(void *ptr)
{
  ST_SCHEMA_TABLE *schema= static_cast<ST_SCHEMA_TABLE *>(ptr);
  schema->fields_info= memory_fields;
  schema->fill_table= memory_fill;
  return 0;
}

static int disk_init(void *ptr)
{
  ST_SCHEMA_TABLE *schema= static_cast<ST_SCHEMA_TABLE *>(ptr);
  schema->fields_info= disk_fields;
  schema->fill_table= disk_fill;
  return 0;
}

} // namespace Vmstat
} // namespace

static struct st_mysql_information_schema vmstat_cpu_descriptor=
{ MYSQL_INFORMATION_SCHEMA_INTERFACE_VERSION };
static struct st_mysql_information_schema vmstat_mem_descriptor=
{ MYSQL_INFORMATION_SCHEMA_INTERFACE_VERSION };
static struct st_mysql_information_schema vmstat_disk_descriptor=
{ MYSQL_INFORMATION_SCHEMA_INTERFACE_VERSION };

maria_declare_plugin(vmstat)
{
  MYSQL_INFORMATION_SCHEMA_PLUGIN,
  &vmstat_cpu_descriptor,
  "VMSTAT_CPU",
  "lefred",
  "Linux CPU statistics from /proc/stat",
  PLUGIN_LICENSE_GPL,
  Vmstat::cpu_init,
  NULL,
  0x0100,
  NULL,
  NULL,
  "1.0",
  MariaDB_PLUGIN_MATURITY_BETA
},
{
  MYSQL_INFORMATION_SCHEMA_PLUGIN,
  &vmstat_mem_descriptor,
  "VMSTAT_MEM",
  "lefred",
  "Linux memory statistics from /proc/meminfo and /proc/vmstat",
  PLUGIN_LICENSE_GPL,
  Vmstat::memory_init,
  NULL,
  0x0100,
  NULL,
  NULL,
  "1.0",
  MariaDB_PLUGIN_MATURITY_BETA
},
{
  MYSQL_INFORMATION_SCHEMA_PLUGIN,
  &vmstat_disk_descriptor,
  "VMSTAT_DISK",
  "lefred",
  "Linux block device statistics from /proc/diskstats",
  PLUGIN_LICENSE_GPL,
  Vmstat::disk_init,
  NULL,
  0x0100,
  NULL,
  NULL,
  "0.2.0",
  MariaDB_PLUGIN_MATURITY_BETA
}
maria_declare_plugin_end;
