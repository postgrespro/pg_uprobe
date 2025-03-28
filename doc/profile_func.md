# Профилирование функций PostgreSQL

- [Введение](#введение)
    - [Типы проб](#типы-проб)
    - [Подходящие функции для мониторинга](#подходящие-функции-для-мониторинга)
- [Установка пробы](#установка-пробы)
- [Информация о пробах](#информация-о-пробах)
    - [Показать установленные пробы](#показать-установленные-пробы)
    - [Результат профилирования для текущего сеанса](#результат-профилирования-для-текущего-сеанса)
        - [TIME. Время выполнения функции](#time-время-выполнения-функции)
        - [HIST. Гистограмма времени выполнения функции](#hist-гистограмма-времени-выполнения-функции)
    - [Результат профилирования для всех сеансов](#результат-профилирования-для-всех-сеансов)
        - [Получить результат профилирования](#получить-результат-профилирования)
- [Удаление пробы](#удаление-пробы)


## Введение

Расширение позволяет устанавливать динамические пробы на функции PostgreSQL в user space и детально исследовать внутреннюю работу СУБД.

### Типы проб

| Проб  |  Имя | Описание  |
|--------|------|-----------|
| TIME   | Время выполнения функции | Данный тип проб позволяет отслеживать среднее время выполнения функции и количество вызовов функции.  |
| HIST   | Гистограмма времени выполнения функции| Собирается информация о времени выполнения функции в виде гистограммы. Это позволяет более детально изучить как работает функция при разных обстоятельствах. |
| MEM    |  Информация о том как изменялась память при вызове функции |  Собирается информация о том как меняется память до входа в функцию и после выхода из неё.|

### Подходящие функции для мониторинга
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

## Установка пробы

Чтобы установить пробу, необходимо воспользоваться следующей ```sql``` функцией:

```set_uprobe(IN func text, IN uprobe_type text, IN is_shared boolean);```

| Аргумент | Описание |
|---|---|
| func |  Название функции из исполняемого файла или подгружаемой библиотеки |
| uprobe_type | Тип пробы <ul><li>TIME -  Время выполнения функции</li><li>HIST -  Гистограмма времени выполнения функции|</li></ul> |
| is_shared | Признак установки пробы на текущий сеанс или на все новые сеансы <ul><li>false -  Проб устанавливается только для текущего сеанса и вся собранная информация также храниться в этом сеансе </li><li>true -  Проб устанавливается для всех новых сеансов. Все новые сеансы сбрасывают собранную информацию в общую память|</li></ul>|

При корректном завершении ```set_uprobe``` возвращает имя func.

## Информация о пробах

### Показать установленные пробы
Чтобы посмотреть все установленные пробы, необходимо воспользоваться следующим запросом:

```select list_uprobes();```

Запрос возвращает набор строк, в каждой строке содержится запись, которая соответствует одному установленному пробу.

Запись имеет вид:

```(func, uprobe_type, is_shared)```

| Аргумент | Описание |
|---|---|
| func |  Название функции из исполняемого файла или подгружаемой библиотеки |
| uprobe_type | см. [типы проб](#типы-проб) |
| is_shared | Признак установки пробы на текущий сеанс или на все новые сеансы <ul><li>false -  Проб устанавливается только для текущего сеанса и вся собранная информация также храниться в этом сеансе </li><li>true -  Проб устанавливается для всех новых сеансов. Все новые сеансы сбрасывают собранную информацию в общую память|</li></ul>|

### Результат профилирования для текущего сеанса

#### TIME. Время выполнения функции
Чтобы получить информации о пробе, установленной на текущем сеансе, необходимо воспользоваться следующей ```sql``` функцией:

```stat_time_uprobe(IN func text)```

| Аргумент | Описание |
|---|---|
| func |  Название функции из исполняемого файла или подгружаемой библиотеки |

При корректном завершении возвращает строку вида: 
```"calls: {количество вызовов функции}  avg time: {среднее время выполнения функции} ns"```.

#### HIST. Гистограмма времени выполнения функции
Чтобы получить гистограмму времени выполнения функции, необходимо воспользоваться следующей ```sql``` функцией:


```stat_hist_uprobe( IN func text, IN start double precision, IN stop double precision, IN step double precision)```

| Аргумент | Описание |
|---|---|
| func |  Название функции из исполняемого файла или подгружаемой библиотеки |
| start | Значение в микросекундах, с которого нужно начать строить гистограмму. Если функция выполнялась меньше указанного значения, то эти времена не попадут в итоговую гистограмму |
| stop | Значение в микросекундах, на котором нужно закончить строить гистограмму. Если функция выполнялась больше указанного значения, то эти времена не попадут в итоговую гистограмму |
| step | Значение в микросекундах, шаг гистограммы |


Если параметры start, stop, step для прошлой функции не известны заранее, то можно воспользоваться функцией, которая подберет их автоматически:


```stat_hist_uprobe( IN func text)```

| Аргумент | Описание |
|---|---|
| func |  Название функции из исполняемого файла или подгружаемой библиотеки |


При корректном завершении возвращает набор строк, который в ```psql``` будет выглядеть как гистограмма, аналогичная выводу ```bpftrace```.

- time_range - Интервал времени
- hist_entry - Строка для красоты забитая '@', один символ за 2% из percent
- percent - Процент измерений, который попал в данный интервал

Примечание: Чтобы гистограмма была выравненной, нужно получать данные из функции, а не вызывать её напрямую. Правильный вариант:
```sql
select * from stat_hist_uprobe('PortalStart');
```

 ### Результат профилирования для всех сеансов

 Основное отличие от профилирования собственного процесса заключается в том, что результат профилирования будет доступен на файловой системе, а не сразу в виде результата функций.

#### Получить результат профилирования
Чтобы получить результаты профилирования для всех сеансов, необходимо воспользоваться следующей функцией:

```dump_uprobe_stat(IN func text, IN should_empty_stat boolean)```

| Аргумент | Описание |
|---|---|
| func |  Название функции из исполняемого файла или подгружаемой библиотеки |
| should_empty_stat | Признак сброса собранной информации. <ul><li>false - Собранная информация не удаляется и продолжает собираться </li><li>true - Собранная информация удаляется</li></ul>|

При корректном завершении в каталоге **pg_uprobe.data_dir** создается файл с собранной информацией. Для каждого [типа проб](#типы-проб) свой формат:

| Тип пробы | Описание файла |
|---|---|
| TIME | Имя файла: TIME_{func}.txt <br> В файле строка  вида: <br>```num calls: {количество вызовов функции}  avg time: {среднее время выполнения функции} nanosec```|
| HIST | Имя файла: HIST_{func}.txt <br> В файле строки  вида: <br> ```time,count``` <ul><li>time - Время в наносекундах сколько работала функция </li><li>count - Сколько раз выполнялась функция с указанным временем</li></ul>|
| MEM | Имя файла: MEM_{func}.txt <br> В файле строки  вида: <br> ```memory,count``` <ul><li>memory - Размер в байтах на сколько менялась выделенная память до входа в функцию и после неё </li><li>count - Сколько раз выполнялась функция с указанным временем</li></ul> |

## Удаление пробы
Чтобы удалить пробу, необходимо воспользоваться следующей функцией:

```delete_uprobe(IN func text, IN should_write_stat boolean)```

| Аргумент | Описание |
|---|---|
| func |  Название функции из исполняемого файла или подгружаемой библиотеки |
| should_write_stat | Только когда профилируем множество сеансов. Если true, то перед удалением пробы собирается результаты профилирования для всех сеансов аналогично функции |

Важно отметить, что сам проб удаляется только на текущем сеансе. С остальных сеансов проб не будет удален, но на новых уже появляться не будет.