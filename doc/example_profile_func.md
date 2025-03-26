# Примеры профилирования функции

Подразумевается, что расширение уже установлено и настроено. Как установить и настроить расширение можно посмотреть в основном файле [README](./../README.md#установка).

Для начала нужно выбрать функцию, которую мы будем профилировать. Без исходного кода PostgreSQL - не обойтись. Исходный код PostgreSQL расположен [тут](https://github.com/postgres/postgres).

Мы будем профилировать функцию `PortalStart`. Функция `PortalStart` подготавливает портал(portal) для выполнения запроса. Портал в контексте баз данных PostgreSQL — это объект, который представляет собой подготовленное состояние для выполнения запроса. Эта функция выполняет необходимую инициализацию перед тем, как портал сможет быть использован для выполнения запроса через вызов `PortalRun`.

Чтобы данные были показательные, давайте сгенерируем БД  и будем подавать нагрузку с помощью [pgbench](https://postgrespro.ru/docs/postgresql/17/pgbench), если установлен PostgreSQL, он устанавливается автоматически .

Генерация данных:
```console
 vadimlakt:~$ pgbench -i -s 100
```

## Профилируем текущий сеанс

В `psql` ставим пробу на функцию `PortalStart`:

```sql
select set_uprobe('PortalStart', 'HIST', false);
```

Проверим, что проба установилась:

```sql
select list_uprobes();
     list_uprobes     
----------------------
 (PortalStart,HIST,f)
(1 строка)

```

Генерируем нагрузку с помощью этого же терминала `psql`:
```sql
select * from pgbench_accounts LIMIT 5;
select * from pgbench_accounts LIMIT 5;
select * from pgbench_accounts LIMIT 5;
select * from pgbench_accounts LIMIT 5;

```
С помощью stat_hist_uprobe_simple построим гистограмму:

```sql
select * from stat_hist_uprobe_simple('PortalStart');
     time_range     |                     hist_entry                     | percent 
--------------------+----------------------------------------------------+---------
 (..., 12.6 us)     |                                                    |   0.000
 (12.6 us, 17.1 us) | @@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@                  |  66.666
 (17.1 us, 21.7 us) | @@@@@@@@                                           |  16.666
 (26.2 us, ...)     |                                                    |   0.000
(4 строки)

```

Удалим пробу, чтобы она не мешала нормальной работе системы:
```sql
select delete_uprobe('PortalStart', false);
```

Проверим, что проба удалена:
```sql
select list_uprobes();
 list_uprobes 
--------------
(0 строк)
```

## Профилируем все сеансы в системе

В `psql` ставим пробу на функцию `PortalStart`:

```sql
select set_uprobe('PortalStart', 'HIST', true);
```
Делаем нагрузку с помощью `pgbench`:
```console
vadimlakt:~$ pgbench -t 100 -P 1 postgres
pgbench (16.8)
starting vacuum...end.
transaction type: <builtin: TPC-B (sort of)>
scaling factor: 100
query mode: simple
number of clients: 1
number of threads: 1
maximum number of tries: 1
number of transactions per client: 100
number of transactions actually processed: 100/100
number of failed transactions: 0 (0.000%)
latency average = 1.215 ms
latency stddev = 0.211 ms
initial connection time = 52.587 ms
tps = 822.787935 (without initial connection time)
```
Удаляем пробу и сразу собираем статистику в `psql`:
```sql
select delete_uprobe('PortalStart', true);
```
При корректном завершении в каталоге **pg_uprobe.data_dir** создается файл с собранной информацией. В нашем случае файл называется `HIST_PortalStart.txt`. Содержимое файла выглядит следующим образом:
```
time,count
0.2,48
0.3,217
0.4,286
0.5,31
0.6,13
0.7,5
0.8,1
1.0,1
1.6,1
5.4,4
5.5,10
5.6,25
5.7,20
5.8,5
5.9,6
6.0,8
6.1,1
6.2,1
6.7,1
6.8,2
6.9,1
7.2,1
7.4,1
7.5,1
8.1,2
8.2,1
8.3,2
8.5,1
9.3,1
10.0,1
10.1,1
10.7,1
10.9,1
11.0,1
11.1,1
118.9,1
71.8,1
```
Для других типов проб всё делается аналогично.