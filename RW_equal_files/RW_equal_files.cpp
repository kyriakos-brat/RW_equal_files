#include "pch.h"
#include <iostream>
#include <fstream>
#include <memory>
#include <thread>
#include <shared_mutex>
#include <vector>
#include <string>
#include <chrono>
#include <atomic>
#include <condition_variable>

//Глобальная переменная, необходима только для 
//выполнения программы в течении 60 секунд 
bool onemin = false;

//Объявление класс должно быть в .h, но для простоты это опущено.
//Реализация шаблонного класса кольцевого буфера 
//Стандартный кольцевой буфер, работает за счет двух указателей head_ и tail_ - первый необходим 
//для опеределения позиции вставки элемента, второй - откуда элемент берется (в схеме с несколькими
//читателями буфера используется другой принцип)
template <class T> class circ_buffer {
public:
	explicit circ_buffer(size_t size) : buf_(std::unique_ptr<T[]>(new T[size])), max_size_(size), reader_check(size, -1), elem_writed(size, false)
	{
		//тело конструктора пусто, все, что он делает - через список
		//инициализации конструктора выделяет необходимое количество памяти,
		//устанавливает размер массива, инициализирует вектор, отвечающий
		//за контроль прочтения ячеек читателем и вектор, 
		//отображающий статус ячейки (доступна для чтения или нет).
		//Для реализации самого буфера можно использовать массив
		//или связный список - выбран первый вариант из-за простоты,
		//времени доступа к элементам за O(1), а также из-за того, что
		//в добавлении новых ячеек буфера нет необходимости.

	}
	//Используется несколько mutex'ов для реализации
	//"атомарного" доступа к векторам и переменным контроля
	//параллельного выполнения конкурирующих процессов
	std::mutex rnum_mut, rc_mut, ew_mut, buf_mut;
	//reader_check - вектор размера самого буфера, хранящий число читателей, которые
	//уже прочитали выбранную ячейку. Необходим для контроля писателя (он может писать только
	//тогда, когда все читатели прочитали ячейку).
	std::vector< int > reader_check;
	//elem_writed - вектор размера самого буфера, необходимый для отслеживания статуса
	//в конкретной ячейке (прочтения которой ждет читатель): появились новые данные и 
	//их необходимо считать или нет.
	std::vector<bool> elem_writed;
	//атомарная переменная, хранящая количество текущих читателей буфера
	std::atomic<int> num_of_readers;
	void add_reader();
	void remove_reader();
	void put(T item);
	T get();
	T get(size_t idx);
	void reset();
	bool empty() const;
	bool full() const;
	size_t capacity() const;
	size_t size() const;

private:
	//Через умный указатель осуществляется доступ к элементам 
	//буфера, данный указатель полностью владеет
	//буфером и ни с кем его не разделяет
	std::unique_ptr<T[]> buf_;
	size_t head_ = 0;
	size_t tail_ = 0;
	const size_t max_size_;
	//Переменная, необходимая для реализации "цикличности" буфера.
	//Для проверки на то, что последняя ячейка буфера заполнена
	//и необходимо вернуться на 0ой элемент, можно использовать два
	//подхода: 1) тратить слот в буфере: буфер "полон" когда head_+1==tail_,
	//при этом последний слот никогда нельзя использовать для хранения;
	//буфер пуст, когда head_==tail_;
	//		   2) использовать bool переменную: буфер "полон" когда она принимает
	//значение true, буфер пуст когда (head_==tail_) && !full (это добавляет 
	//некоторый код в put())
	bool full_ = 0;
};


template<class T>
void circ_buffer<T>::add_reader()
{
	rnum_mut.lock();
	num_of_readers += 1;
	rnum_mut.unlock();
}

template<class T>
void circ_buffer<T>::remove_reader()
{
	rnum_mut.lock();
	num_of_readers -= 1;
	rnum_mut.unlock();
}

//Добавление данных в буфер.
//Добавление (как и удаление) требует манипуляций с переменными
//head_ и tail_. При добавлении данных в буфер мы вставляем
//значение в текущую позицию head_, затем "продвигая" head_.
//В добавлении также имеется манипулирование с переменной
//full. Если буфер "полон", нам необходимо продвинуть и 
//head_, и tail_. Также необходима проверка, изменяет ли текущая
//вставка состояние full.
//Используется оператор деления по модулю для того, чтобы
//переменные head_ и tail_ принимали значение 0 после достижения
//максимального индекса буфера.
template<class T>
void circ_buffer<T>::put(T item)
{
	//Пытаемся получить эксклюзивный доступ к вектору reader_check
	if (rc_mut.try_lock()) {
		//Писатель может писать только если все читатели прочитали ячейку
		//или это первый "проход" (по умолчанию все значения вектора reader_check == -1)
		if ((reader_check[head_] == num_of_readers) || (reader_check[head_] == -1)) {
			//В ячейке появилась новые данные, их еще никто не прочитал
			reader_check[head_] = 0;

			buf_[head_] = item;
			if (full_) {
				tail_ = (tail_ + 1) % max_size_;
			}
			head_ = (head_ + 1) % max_size_;
			full_ = head_ == tail_;
			std::cout << "Data writed at " << (head_ - 1) << std::endl;

			//Меняем статус ячейки на "появилась новая информация, ее нужно прочитать"
			ew_mut.lock();
			elem_writed[head_] = true;
			ew_mut.unlock();
			rc_mut.unlock();
			std::this_thread::sleep_for(std::chrono::nanoseconds(25));
		}
		else { //Если еще не все прочитали ячейку - отпускаем lock на ячейку, ждем читателей
			rc_mut.unlock();
			std::this_thread::sleep_for(std::chrono::nanoseconds(25));
		}
	}
	std::this_thread::sleep_for(std::chrono::nanoseconds(25));
}

//Получаение элмента из буфера с его удалением.
//При получении элемента из буфера мы получаем
//значение из tail_ позиции буфера, продвигая ее.
template<class T>
T circ_buffer<T>::get()
{
	if (empty()) {
		return T();
	}

	auto val = buf_[tail_];
	full_ = false;
	tail_ = (tail_ + 1) % max_size_;

	return  val;
}

//Простое чтение из буфера, без удаления прочитанного элемента.
template<class T>
T circ_buffer<T>::get(size_t idx)
{
	idx = idx % max_size_;
	//Проверяем статус ячейки - появились ли в ней новые данные
	ew_mut.lock();
	bool check = elem_writed[idx];
	ew_mut.unlock();
	while (!check && !onemin) { //Если новых данных не появилось - ждем их появления
		std::cout << "Elem is busy, waiting... reader_check:" << reader_check[idx] << " numof: " << num_of_readers << std::endl;
		std::this_thread::sleep_for(std::chrono::nanoseconds(10));
		ew_mut.lock();
		check = elem_writed[idx];
		ew_mut.unlock();
	}
	if (empty()) {
		/*rc_mut.lock();
		reader_check[idx] += 1;
		rc_mut.unlock();*/
		return T();
	}
	//Получаем доступ к ячейке (эксклюзивный доступ тут не нужен - то, что данные в ней
	//не изменятся в ходе ее прочтения гарантирует reader_check
	auto val = buf_[idx];
	//Увеличиваем количество читателй, прочитавших ячейку
	rc_mut.lock();
	reader_check[idx] += 1;
	rc_mut.unlock();

	//Если все читатели прочитали ячейку - ее статус изменяется на
	//"в ячейке новая информация отсутствует"
	rc_mut.lock();
	if (reader_check[idx] == num_of_readers) {
		ew_mut.lock();
		elem_writed[idx] = false;
		ew_mut.unlock();
		std::cout << "Elem_writed reset" << std::endl;
	}
	rc_mut.unlock();
	return val;
}

//Делает буфер "пустым". 
//Старые значение не берутся в учет, в виду цикличности
//буфера не имеет значения с какого элемента начинать запись,
//поэтому старые значения не удалются, а просто перезаписываются
//в ходе работы с буфером.
template <class T>
void circ_buffer<T>::reset() {
	head_ = tail_;
	full_ = false;
}

//Проверка буфера на пустоту.
template<class T>
bool circ_buffer<T>::empty() const
{
	return (!full_ && (head_ == tail_));
}

template<class T>
bool circ_buffer<T>::full() const
{
	return full_;
}

//Возвращает максимальный размер буфера.
template<class T>
size_t circ_buffer<T>::capacity() const
{
	return max_size_;
}

//Возвращает текущую заполненность буфера.
//Если буфер полон - мы знаем, что размер буфера - максимальный
//Если head_ >= tail_, то мы просто вычитаем, чтобы получить размер.
//Если head < tail, нам необходимо "отсечь" разницу с максимально
//возможным размером, чтобы получить правильный размер.
template<class T>
size_t circ_buffer<T>::size() const
{
	size_t size = max_size_;

	if (!full_) {
		if (head_ >= tail_) {
			size = head_ - tail_;
		}
		else {
			size = max_size_ + head_ - tail_;
		}
	}
	return size;
}

//Производит чтение данных из буфера.
//В качестве максимальной длины кадра выбрана
//длина 1 байт - поэтому реализация сделана под
//char(пусть и не все компиляторы действительно 
//хранят char в 1 байте, тем не менее все они гарантируют,
//что sizeof(char)==1 байт
template<class T>
void read_data(circ_buffer<T> *pC, size_t num)
{
	pC->add_reader();
	std::ofstream out;
	std::string filename_("file");
	filename_.append(std::to_string(num));
	filename_.append(".txt");
	out.open(filename_, std::ios::out | std::ios::binary);
	unsigned long long int i = 0;
	if (out.is_open()) {
		T w;
		while (!onemin) {
			w = pC->get(i);
			out.write(&w, 1);
			std::cout << "Read_data returned: " << w << std::endl;
			++i;
		}
		out.close();
		pC->remove_reader();
	}
}

//Производит запись данных в буфер.
//В качестве данных, записываемых в буфер выбраны ASCII символы
//в диапазоне от 30 до 128, но это значение может быть любым
template<class T>
void write_data(circ_buffer<T>  *pC) {
	unsigned long long i = 71;
	while (!onemin) {
		pC->put(i);
		i = ((i + 1) % 99) + 30;
	}
}

//Проверяет время работы программы.
//При достижении длительности работы программы (отсчет начинается
//после ввода исходных данных) в 60 секунд приводит к последовательному
//завершнению всех потоков
void check_time() {
	auto start = std::chrono::steady_clock::now();
	auto current_time = std::chrono::steady_clock::now();
	std::chrono::duration<double> elasped_time = current_time - start;
	while (elasped_time.count() < 60) {
		std::this_thread::sleep_for(std::chrono::seconds(1));
		current_time = std::chrono::steady_clock::now();
		elasped_time = current_time - start;
		std::cout << "Check time " << std::this_thread::get_id() << std::endl;
	}
	onemin = true;
	std::cout << "Done " << elasped_time.count() << std::endl;
}

int main()
{
	size_t buf_size;
	std::cin >> buf_size;
	size_t readers_num;
	std::cin >> readers_num;
	circ_buffer<char> circle(buf_size);
	std::vector<std::thread> writers;
	std::thread tW(write_data<char>, &circle);
	std::thread time_thread(check_time);
	for (size_t i = 0; i < readers_num; ++i)
		writers.push_back(std::thread(read_data<char>, &circle, i));
	tW.join();
	time_thread.join();
	for (auto &writer : writers)
		writer.join();
	return 0;
}
