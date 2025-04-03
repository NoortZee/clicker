#include <windows.h>
#include <chrono>
#include <thread>
#include <fstream>
#include <vector>
#include <string>
#include <iostream>
#include <atomic>
#include <mutex>
#include <conio.h> // Для функций _kbhit и _getch

// Глобальная переменная для отслеживания нажатия F9
std::atomic<bool> should_exit(false);
std::mutex cout_mutex;

// Константы для калибровки точного ожидания
static double sleep_overhead = 0.5; // миллисекунды
static double spin_correction = 0.98;

// Функция для вывода в консоль с защитой от конкурентного доступа
void safe_cout(const std::string& message) {
    std::lock_guard<std::mutex> lock(cout_mutex);
    std::cout << message << std::endl;
}

// Функция для мониторинга нажатия клавиши F9
void monitor_f9_key() {
    while (!should_exit.load()) {
        if (GetAsyncKeyState(VK_F9) & 0x8000) {
            should_exit.store(true);
            safe_cout("F9 pressed. Stopping execution...");
            break;
        }
        // Короткая пауза для снижения нагрузки на CPU
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
}

// Функция для точного ожидания с гибридным подходом
void precise_sleep(long long microseconds) {
    // Добавляем коррекцию на основе предыдущего опыта
    double target_time = microseconds * (1.0 + (microseconds < 10000 ? 0.002 : 0.001));
    
    LARGE_INTEGER freq, start, current;
    QueryPerformanceFrequency(&freq);
    QueryPerformanceCounter(&start);
    
    // Конвертируем в счетчики производительности
    double target_counts = start.QuadPart + (freq.QuadPart * target_time) / 1000000.0;
    
    // Порог для переключения на активное ожидание (500 микросекунд)
    double spin_threshold = start.QuadPart + (freq.QuadPart * 500) / 1000000.0;
    
    // Фаза 1: Sleep для большей части ожидания
    while (!should_exit.load()) {
        QueryPerformanceCounter(&current);
        if (current.QuadPart >= target_counts) return;
        
        // Если осталось меньше порога для активного ожидания, переходим к фазе 2
        if (current.QuadPart >= spin_threshold) break;
        
        // Рассчитываем оставшееся время для Sleep
        double remaining = (target_counts - current.QuadPart) * 1000.0 / freq.QuadPart;
        
        // Вычитаем overhead чтобы не переспать
        remaining = max(0.0, remaining - sleep_overhead);
        
        if (remaining > 1.0) {
            Sleep(static_cast<DWORD>(remaining * spin_correction));
        } else {
            Sleep(0);
        }
    }
    
    // Фаза 2: Активное ожидание для финальной точности
    while (!should_exit.load()) {
        QueryPerformanceCounter(&current);
        if (current.QuadPart >= target_counts) break;
        
        // Критически важная точка - чистый спинлок без уступок процессора
        // Используем _mm_pause или эквивалент, чтобы не перегружать ядро
        YieldProcessor();
    }
}

// Функция для калибровки точности ожидания (добавить в начало main)
void calibrate_sleep() {
    safe_cout("Калибровка точного ожидания...");
    const int test_count = 5;
    
    // Калибровка Sleep overhead
    double total_overhead = 0;
    for (int i = 0; i < test_count; i++) {
        LARGE_INTEGER freq, start, end;
        QueryPerformanceFrequency(&freq);
        QueryPerformanceCounter(&start);
        Sleep(1);
        QueryPerformanceCounter(&end);
        
        double actual_sleep = (end.QuadPart - start.QuadPart) * 1000.0 / freq.QuadPart;
        total_overhead += (actual_sleep - 1.0);
    }
    sleep_overhead = total_overhead / test_count;
    
    // Калибровка коэффициента коррекции для spin
    double total_correction = 0;
    for (int i = 0; i < test_count; i++) {
        LARGE_INTEGER freq, start, end;
        QueryPerformanceFrequency(&freq);
        QueryPerformanceCounter(&start);
        precise_sleep(50000); // 50 мс
        QueryPerformanceCounter(&end);
        
        double expected = 50.0;
        double actual = (end.QuadPart - start.QuadPart) * 1000.0 / freq.QuadPart;
        if (actual > 0) total_correction += expected / actual;
    }
    spin_correction = (total_correction / test_count) * spin_correction; // Адаптивная коррекция
    
    safe_cout("Калибровка завершена: overhead=" + std::to_string(sleep_overhead) + 
              "мс, correction=" + std::to_string(spin_correction));
}

// Функция для имитации клика мыши (замените на свою реализацию)
void mouse_click_left() {
    INPUT input = { 0 };
    input.type = INPUT_MOUSE;
    input.mi.dwFlags = MOUSEEVENTF_LEFTDOWN;
    SendInput(1, &input, sizeof(INPUT));

    ZeroMemory(&input, sizeof(INPUT));
    input.type = INPUT_MOUSE;
    input.mi.dwFlags = MOUSEEVENTF_LEFTUP;
    SendInput(1, &input, sizeof(INPUT));
}

// Функция для чтения команд из файла
std::vector<std::pair<long long, std::string>> read_commands(const std::string& filename) {
    std::vector<std::pair<long long, std::string>> commands;
    std::ifstream file(filename);
    std::string line;

    while (std::getline(file, line)) {
        if (line.find("wait") != std::string::npos) {
            size_t space_pos = line.find(' ');
            if (space_pos != std::string::npos) {
                std::string delay_str = line.substr(space_pos + 1);
                // Удаляем точку с запятой, если она есть
                if (delay_str.back() == ';') {
                    delay_str.pop_back();
                }
                long long delay = std::stoll(delay_str);
                commands.emplace_back(delay, "wait");
            }
        } 
        else if (line.find("click") != std::string::npos) {
            commands.emplace_back(0, "mouse_click_left");
        }
    }

    return commands;
}

// Функция для выполнения команд
void execute_commands(const std::vector<std::pair<long long, std::string>>& commands) {
    for (const auto& cmd : commands) {
        if (should_exit.load()) break;

        if (cmd.second == "wait") {
            precise_sleep(cmd.first * 1000); // Конвертируем миллисекунды в микросекунды
        }
        else if (cmd.second == "mouse_click_left") {
            mouse_click_left();
        }
        // Добавьте другие команды по мере необходимости
    }
}

int main() {
    std::cout << "Press F8 to start executing commands from file..." << std::endl;
    std::cout << "Press F9 anytime to stop execution" << std::endl;

    // Запуск калибровки для точного хронометража
    calibrate_sleep();

    while (true) {  // Бесконечный цикл для повторного запуска
        // Ожидание нажатия F8
        while (true) {
            if (GetAsyncKeyState(VK_F8) & 0x8000) {
                break;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }

        // Сбрасываем флаг остановки
        should_exit.store(false);

        std::cout << "F8 pressed, reading commands from file..." << std::endl;

        // Чтение команд из файла
        std::string filename = "commands.txt";
        std::ifstream file(filename);
        if (!file.is_open()) {
            std::cout << "Error: Cannot open file " << filename << std::endl;
            std::cout << "Press any key to exit..." << std::endl;
            std::cin.get();
            return 1;
        }
        file.close();
        
        auto commands = read_commands(filename);
        
        if (commands.empty()) {
            std::cout << "No valid commands found in file!" << std::endl;
            std::cout << "Press any key to exit..." << std::endl;
            std::cin.get();
            return 1;
        }
        
        std::cout << "Found " << commands.size() << " commands. Starting execution..." << std::endl;
        std::cout << "Press F9 to stop execution at any time" << std::endl;

        // Запуск потока мониторинга клавиши F9
        std::thread f9_monitor_thread(monitor_f9_key);

        // Выполнение команд
        auto start = std::chrono::high_resolution_clock::now();
        execute_commands(commands);
        auto end = std::chrono::high_resolution_clock::now();

        // Завершаем мониторинг клавиши F9, если он еще не завершился
        should_exit.store(true);
        if (f9_monitor_thread.joinable()) {
            f9_monitor_thread.join();
        }

        std::string exitMessage = should_exit.load() ? "Execution stopped by user (F9)" : "All commands executed";
        std::cout << exitMessage << " in "
            << std::chrono::duration_cast<std::chrono::milliseconds>(end - start).count()
            << " ms" << std::endl;
        
        std::cout << "Press F8 to run again or any other key to exit..." << std::endl;
        
        // Небольшая задержка перед проверкой клавиш, чтобы избежать "залипания" F8
        std::this_thread::sleep_for(std::chrono::milliseconds(500));
        
        // Проверяем, хочет ли пользователь выйти
        bool exit_requested = false;
        for (int i = 0; i < 100 && !exit_requested; i++) {  // Даем около 1 секунды на решение
            if (GetAsyncKeyState(VK_F8) & 0x8000) {
                // Пользователь нажал F8, продолжаем цикл
                break;
            }
            if (_kbhit()) {  // Если нажата любая другая клавиша
                exit_requested = true;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
        
        if (exit_requested) {
            break;  // Выход из главного цикла
        }
        
        // Очистка буфера клавиатуры
        while (_kbhit()) {
            _getch();
        }
    }

    return 0;
}