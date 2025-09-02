
- [pg_uprobe](#pg_uprobe)
  - [Concept](#concept)
    - [Session Tracing](#session-tracing)
    - [Profiling PostgreSQL Functions](#profiling-postgresql-functions)
  - [Limitations](#limitations)
    - [Session Tracing](#session-tracing-1)
    - [Profiling PostgreSQL Functions](#profiling-postgresql-functions-1)
  - [Installation](#installation)
    - [Build](#build)
    - [Configuration](#configuration)
    - [Creating the Extension](#creating-the-extension)
  - [Tests] (#tests)
  - [Settings](#settings)
  - [Usage](#usage)
  - [Usage Examples](#usage-examples)
  - [Code branching model](#code-branching-model)

# pg_uprobe
A PostgreSQL extension designed for tracing and analyzing queries executed within a session. This extension allows to capture and log information about running queries inside session. The collected information can retrieve heavy operations and most consumed resources  during the execution of specific SQL queries.

For advanced users familiar with PostgreSQL's internal architecture, there is an option to set dynamic probes on C functions to examine the internals of the DBMS.

## Concept

Under the hood, pg_uprobe utilizes one of the best code analysis tools  - [Frida](https://frida.re) and [Frida Gum](https://github.com/frida/frida-gum) library, which is part of the Frida ecosystem and provides a low-level API for dynamic code injection.

Using the extension does not require patching or modifying PostgreSQL's source code. The Frida toolkit allows dynamically injecting the necessary code into a running PostgreSQL instance.

### Session Tracing

Once a connection to the DBMS is established, the extension can be used to enable tracing of all queries executed within session. Tracing can be enabled for your session using the start_session_trace() function, or for other sessions, such as those created by an application.

The collected information can reveal the following:

- Query text
- Time spent parsing the query
- Query plan
- Query plan type (generic, custom)
- Time spent planning the query
- Time spent executing the entire query
- Time spent executing each plan node
- Time spent waiting for events (wait events). For example: file reads, lock waits
- Shared memory locks acquired during query execution

This information is collected for every query executed during tracing.

### Profiling PostgreSQL Functions

If you are familiar with PostgreSQL's source code, our extension allows setting dynamic probes on some (see the "Limitations" section)  C functions inside PostgreSQL core. Functions can be profiled either for your own session or for all sessions created after the probe is set. We have prepared several types of probes that can be installed.

Probe types:
- TIME - Measures the time spent executing the function
- HIST - Measures the time spent executing the function and builds a histogram of execution times
- MEM - Measures the change in PostgreSQL memory (MemoryContext) before and after the function execution

## Limitations

Supported PostgreSQL versions:

- PostgreSQL 15/16/17
- [Postgres Pro Standard 15/16/17](https://postgrespro.ru/products/postgrespro/standard)
- [Postgres Pro Enterprise 15/16/17](https://postgrespro.ru/products/postgrespro/enterprise)

Supported architectures:

|Informal name|Name in RPM and Linux kernel|Name in Debian and Astra|Features of hardware platform support|
|------------|------------|------------|------------|
|Intel compatible 64-bit|x86_64|amd64||
|ARM 64-bit|aarch64|arm64|| Not tested, but Frida library supports this architecture

(We are working on supporting more architectures)

Supported operating systems:

- Linux
- FreeBSD

### Session Tracing

When tracing sessions, query execution time may increase. In our measurements, performance drops by ~5%. Therefore, session tracing should not be left enabled for extended periods. This tool is primarily intended for investigating issues, not detecting them.

### Profiling PostgreSQL Functions

Unfortunately, we cannot profile all PostgreSQL functions. A function must meet certain criteria:
- Not Inline
- Not Static
- i.e the function must be present in the ELF file

To check whether your desired function is in the ELF file, you can use the following commands (for instance Postgres Pro Enterprise 15):

**objdump**:
```shell
objdump -T /opt/pgpro/ent-15/bin/postgres
objdump -T /opt/pgpro/ent-15/bin/postgres | awk '{ print $7 }'
```

**readelf**:
```shell
readelf -s -W /opt/pgpro/ent-15/bin/postgres
readelf -s -W /opt/pgpro/ent-15/bin/postgres | awk '{ print $8 }'
```

**nm**:
```shell
nm -D --demangle /opt/pgpro/ent-15/bin/postgres
nm -D --demangle /opt/pgpro/ent-15/bin/postgres | awk '{ print $NF }'
```

Where `/opt/pgpro/ent-15/bin/postgres` is the path to the installed Postgres Pro/PostgreSQL binaries.

## Installation

Currently, the extension can only be built manually. There is no prebuilt packages.

Requirements:
- gcc
- CMake >= 3.15
- python

The installation process involves the following steps:

- Download Frida library
- Build the extension itself

### Build

```shell
git clone https://github.com/postgrespro/pg_uprobe
cd pg_uprobe
make USE_PGXS=1 PG_CONFIG=/opt/pgpro/ent-15/bin/pg_config install
```

Note: `/opt/pgpro/ent-15/bin/pg_config` is the path to the installed pg_config application, which is stored in the same location as PostgreSQL.

### Configuration

In the `$PGDATA/postgresql.conf` file, add the pg_uprobe extension to the list of shared libraries loaded at server startup:

```shell
shared_preload_libraries = 'pg_uprobe'	# (change requires restart)
```
After this, restart the PostgreSQL cluster.

### Creating the Extension

```sql
postgres=# CREATE EXTENSION pg_uprobe;
```
If you need to install it in a different schema, simply create the schema and install the extension there:

```sql
postgres=# CREATE SCHEMA uprobe;
postgres=# CREATE EXTENSION pg_uprobe SCHEMA uprobe;
```
We recommend installation in a dedicated schema. All objects will be created in the schema specified by the `SCHEMA` clause. If you do not want to specify the schema qualifier when using the extension, consider modifying the `search_path` parameter.

## Tests
The tests are written in Python using the testgres framework. To run the tests, you need to install the testgres package for Python and set the PG_CONFIG environment variable to the path to the pg_config executable of your PostgreSQL installation.
Running tests:

```shell
make PG_CONFIG=/opt/pgpro/ent-15/bin/pg_config python_tests
```

## Settings

- **pg_uprobe.data_dir** - Path to the directory where the session trace results file and function profiling results files are created. Default: `$PGDATA/pg_uprobe`
- **pg_uprobe.trace_file_name** - Name of the file for session trace results. Default: `trace_file.txt`
- **pg_uprobe.trace_file_limit** - Limit in megabytes for the session trace results file. Default: 16 MB
- **pg_uprobe.trace_write_mode** - Output format for session tracing. Supported values: "text", "json". Default: json
- **pg_uprobe.trace_lwlocks_for_each_node** - If `true`, LWLock statistics will be reset after each `Executor Node` execution; otherwise, statistics will be reset after the `PortalRun` function completes. Default: `true`
- **pg_uprobe.write_only_sleep_lwlocks_stat** - If true, LWLock statistics will only be written in case of lock waits; otherwise, statistics for all acquired LWLocks will be written. Default: true


## Usage

Documentation for [session tracing](doc/trace_session.md)

Documentation for [profiling PostgreSQL functions](doc/profile_func.md)

## Usage Examples

Simple example of [session tracing](doc/example_trace_session.md)

Simple example of [profiling PostgreSQL functions](doc/example_profile_func.md)

## Code branching model

**GitFlow** is used as the main code branching model in the git repository.

==============================<<<RU>>>==============================

# pg_uprobe

Расширение PostgreSQL, предназначено для трассирования и анализа запросов, выполняемых в рамках сеанса. С помощью этого расширения можно захватывать и регистрировать информацию о запросах, которые были выполнены в процессе мониторинга. Собранную информацию можно детально исследовать, чтобы понять, на какие операции и ресурсы уходит время при выполнении конкретных SQL-запросов.

Для более продвинутых пользователей, которые знакомы с внутренним устройством PostgreSQL, есть возможность устанавливать динамические пробы в user space и также детально исследовать внутреннюю работу СУБД.

## Концепция

Под капотом pg_uprobe использует один из лучших инструментов для анализа кода - [Frida](https://frida.re), а также библиотеку [Frida Gum](https://github.com/frida/frida-gum), которая является частью экосистемы Frida и предоставляет низкоуровневый API для работы с динамическим внедрением кода.

Для использования расширения не требуется применять патчи или каким-либо образом изменять исходный код PostgreSQL. Инструментарий Frida позволяет динамически внедрять необходимый код в уже работающий PostgreSQL.

### Трассирование сеансов

Когда подключение к СУБД уже создано, с помощью расширения можно включить трассирование всех запросов, которые будут выполняться в рамках этого сеанса. Включить трассирование можно как для своего сеанса, используя функцию start_session_trace(), так и для трассирования других сеансов, например сеансов, созданных приложением.

По собранной информации можно определить следующее:

- Текст запроса
- Время затраченное на разбор запроса
- План запроса
- Тип плана запроса(generic, custom)
- Время, затраченное на планирование запроса
- Время, затраченное на выполнение всего запроса
- Время, затраченное на выполнение каждого узла плана
- Время, затраченное на ожидание событий(wait events). Например: чтение файлов, ожидание блокировки
- Блокировки в разделяемой памяти, захваченные во время выполнения запроса

Эта информация будет собрана для каждого запроса, выполнявшегося в момент трассирования.

### Профилирование функций PostgreSQL

Если вы знакомы с исходным кодом PostgreSQL, наше расширение позволяет устанавливать  динамические пробы в user space на некоторые(см. раздел "Ограничения") функции PostgreSQL. Профилировать функции можно как для своего сеанса, так и для всех сеансов, которые будут созданы после установки пробы. Мы подготовили несколько типов проб, которые можно установить.

Типы проб:
- TIME - Измеряет, сколько времени мы затратили на выполнение функции
- HIST - Измеряет, сколько времени мы потратили на выполнение функции, и после строится гистограмма по времени выполнения
- MEM - Измеряет, на сколько изменилась память PostgreSQL(MemoryContext) до выполнения функции и после

## Ограничения

Поддерживаемые версии PostgreSQL:
- PostgreSQL 15/16/17
- [Postgres Pro Standard 15/16/17](https://postgrespro.ru/products/postgrespro/standard)
- [Postgres Pro Enterprise 15/16/17](https://postgrespro.ru/products/postgrespro/enterprise)

Поддерживаемые архитектуры: 

|Неформальное название|Название в RPM и Linux kernel|Название в Debian и Astra|Особенности поддержки аппаратных платформ|
|------------|------------|------------|------------|
|Интел-совместимые 64-бит|x86_64|amd64||
|ARM 64-битные|aarch64|arm64|| Не тестировалось, но библиотека Frida поддерживает данную архитектуру

(Мы работаем над поддержкой большего количества архитектур)

Поддерживаемые операционные системы:
- Linux
- FreeBSD

### Трассирование сеансов
При трассировании сеансов время выполнения запросов может увеличиться. В наших измерениях скорость выполнения падает на ~5%. Поэтому не стоит оставлять трассирование сеансов на длительное время. Этот инструмент предназначен, в первую очередь, для исследования проблемы, а не её обнаружения.

### Профилирование функций PostgreSQL
К сожалению, мы не можем профилировать все функции PostgreSQL. Функция должна обладать рядом свойств:

- Не Inline
- Не Static
- Другими словами, функция должна находиться в ELF файле

Чтобы проверить, находится ли необходимая вам функция в ELF файле, можно воспользоваться следующими командами:

**objdump**:
```shell
objdump -T /opt/pgpro/ent-15/bin/postgres
objdump -T /opt/pgpro/ent-15/bin/postgres | awk '{ print $7 }'
```

**readelf**:
```shell
readelf -s -W /opt/pgpro/ent-15/bin/postgres
readelf -s -W /opt/pgpro/ent-15/bin/postgres | awk '{ print $8 }'
```

**nm**:
```shell
nm -D --demangle /opt/pgpro/ent-15/bin/postgres
nm -D --demangle /opt/pgpro/ent-15/bin/postgres | awk '{ print $NF }'
```
Где /opt/pgpro/ent-15/bin/postgres - путь до установленных бинарников PostgreSQL.


## Установка
На данный момент есть возможность установить расширения только собрав его непосредственно на машине где установлена СУБД.

Требования:
- gcc
- CMake >= 3.15
- python

В процессе установки будут выполнены следующие шаги:
- Скачиваются библиотека Frida
- Собирается само расширение

### Сборка

```shell
git clone https://github.com/postgrespro/pg_uprobe
cd pg_uprobe
make USE_PGXS=1 PG_CONFIG=/opt/pgpro/ent-15/bin/pg_config install
```

### Настройка

Где `/opt/pgpro/ent-15/bin/pg_config` — путь до установленного приложения pg_config, который хранится там же, где установлен PostgreSQL.

В файле `$PGDATA/postgresql.conf` необходимо добавить расширение pg_uprobe в список разделяемых библиотек, которые будут загружаться при запуске сервера:

```shell
shared_preload_libraries = 'pg_uprobe'	# (change requires restart)
```

После этого необходимо перезапустить кластер PostgreSQL.

### Создание расширения
```sql
postgres=# CREATE EXTENSION pg_uprobe;
```
Если необходимо установить в другую схему, просто создайте её, и установите расширение в эту схему:

```sql
postgres=# CREATE SCHEMA uprobe;
postgres=# CREATE EXTENSION pg_uprobe SCHEMA uprobe;
```
Все объекты будут созданы в схеме, определенной предложением SCHEMA. Рекомендуется установка в выделенную схему где расширение создаст свои собственные функции. Если вы не хотите указывать квалификатор схемы при использовании расширения, рассмотрите возможность изменения параметра search_path.

## Способы тестирования
Тесты написаны на языке Python с использованием фреймворка testgres. Для запуска тестов необходимо установить пакет testgres для Python и установить переменную окружения PG_CONFIG в путь до исполняемого файла pg_config вашего установленного PostgreSQL.
Запуск тестов:

```shell
make PG_CONFIG=/opt/pgpro/ent-15/bin/pg_config python_tests
```

## Настройки

- **pg_uprobe.data_dir** - Путь к каталогу, в котором будет создаваться файл с результатами трассирования сеанса и файлы с результатами профилирования функций. По умолчанию: `$PGDATA/pg_uprobe`
- **pg_uprobe.trace_file_name** - Имя файла для результатов трассирования сеанса. По умолчанию: `trace_file.txt`
- **pg_uprobe.trace_file_limit** - Лимит в мегабайтах для файла с результатами трассирования сеанса. По умолчанию: 16 МБ
- **pg_uprobe.trace_write_mode** - Формат вывода информации для трассирования сеанса. Поддерживаемые значения: "text", "json". По умолчанию: `json`
- **pg_uprobe.trace_lwlocks_for_each_node** - Если `true`, статистика по LWLock будет сбрасываться после выполнения каждого `Executor Node;`, иначе статистика будет сбрасываться после завершения функции `PortalRun`. По умолчанию: `true`
- **pg_uprobe.write_only_sleep_lwlocks_stat** - Если `true`, статистика по LWLock будет писаться только в случае ожидания блокировки, иначе будет писаться статистика по всем LWLock, которые были захвачены. По умолчанию: `true`

## Использование

Документация по [трассированию сеансов](doc/trace_session.md)

Документация по [профилированию функций PostgreSQL](doc/profile_func.md)

## Примеры использования

Простой пример использования [трассирования сеанса](doc/example_trace_session.md)

Простой пример использования [профилирования функций PostgreSQL](doc/example_profile_func.md)

## Модель ветвления кода

В качестве основной модели ветвления кода в git-репозитории используется **gitFlow**