# RW_equal_files
Программа, реализующая многопоточное приложение. 
В круговом (циклическом) буфере реализован поток-писатель (пишет поток байтов в буфер) и потоки-читатели (количество неограничено).
Программа работает одну минуту, реализует синхронизацию записи и чтения таким образом, что на выходе получаются одинаковые файлы.
