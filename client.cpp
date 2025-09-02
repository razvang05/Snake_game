#include <bits/stdc++.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <iostream>
#include <ncurses.h>
#include <signal.h>

#define PORT 65432
#define SERVER_IP "127.0.0.1"
#define WIDTH 20
#define HEIGHT 15


long long nowSec() { return time(nullptr); }
long long nextPing = 0;
long long lastPong = 0;


using namespace std;

// modul starii de joc pe client
struct GameState {
    int score = 0;
    pair<int,int> fruit{0,0};
    vector<pair<int,int>> snake;
    bool running = true;
    mutex mtx;
    pair<int,int> localDir{1,0};
    bool hasServerState = false;
    bool won = false;
} state;

// deseneaza jocul in ncurses
void drawGame() {
    clear();

    // Desenăm pereții
    for(int x=0;x<WIDTH+2;x++) mvprintw(0, x, "#");
    for(int y=1;y<=HEIGHT;y++) {
        mvprintw(y, 0, "#");
        mvprintw(y, WIDTH+1, "#");
    }
    for(int x=0;x<WIDTH+2;x++) mvprintw(HEIGHT+1, x, "#");

    // Desenăm fructul
    mvprintw(state.fruit.second+1, state.fruit.first+1, "F");

    // Desenăm snake-ul
    for(size_t i=0;i<state.snake.size();i++) {
        auto [x,y] = state.snake[i];
        mvprintw(y+1, x+1, i==0 ? "O" : "o");
    }

    // Scor
    mvprintw(HEIGHT+3, 0, "Score: %d", state.score);
    refresh();
}

// ascultă mesaje de la server
void listenServer(int sock) {
    char buffer[1024];
    string carry; // păstrează resturi de linie între read-uri

    while (true) {
        memset(buffer, 0, sizeof(buffer));
        // citim mesaje de la server
        int valread = read(sock, buffer, sizeof(buffer)-1);
        if (valread <= 0) break;

        carry.append(buffer, valread);

        size_t pos;
        // procesăm toate liniile complete
        while ((pos = carry.find('\n')) != string::npos) {
            string line = carry.substr(0, pos);
            carry.erase(0, pos + 1);
            if (!line.empty() && line.back() == '\r') line.pop_back();

            // parseaza scorul, fructul si pozitiile snake-ului
            if (line.rfind("UPDATE", 0) == 0) {
                int score, fx, fy;
                int x0,y0, x1,y1, x2,y2, x3,y3;
                int readc = sscanf(line.c_str(),
                    "UPDATE %d %d %d %d %d %d %d %d %d %d %d",
                    &score, &fx, &fy,
                    &x0,&y0, &x1,&y1, &x2,&y2, &x3,&y3
                );
                // actualizează starea jocului doar dacă am citit corect toate câmpurile
                if (readc == 11) {
                    lock_guard<mutex> lock(state.mtx);
                    state.score = score;
                    state.fruit = {fx, fy};
                    state.snake.clear();
                    state.snake.push_back({x0,y0});
                    state.snake.push_back({x1,y1});
                    state.snake.push_back({x2,y2});
                    state.snake.push_back({x3,y3});
                    state.hasServerState = true;
                }
                // desenăm jocul
                drawGame();
                // verifică dacă am câștigat
                if (state.score >= 100) {
                    mvprintw(HEIGHT+5,0,"YOU WIN!");
                    refresh();
                    state.running = false;
                }
            }
            // tratează deconectarea
            else if (line.rfind("DISCONNECT", 0) == 0) {
                // afisează motivul (lose/win/altceva)
                if (line.find("reason=lose") != string::npos)
                    mvprintw(HEIGHT+5,0,"YOU LOSE!");
                else if (line.find("reason=win") != string::npos)
                    mvprintw(HEIGHT+5,0,"YOU WIN!");
                else
                    mvprintw(HEIGHT+5,0,"Disconnected: %s", line.c_str());
                refresh();
                state.running = false;
            }
            // tratează PONG
            else if (line.rfind("PONG", 0) == 0) {
                lastPong = nowSec();
            }
            

            if (!state.running) break;
        }

        if (!state.running) break;
    }
}


int main() {
    // creăm socket-ul client și ne conectăm la server
    int sock = socket(AF_INET, SOCK_STREAM, 0);
    if(sock<0) {
        perror("socket");
        return 1;
    }

    // completăm structura de adresă a serverului
    sockaddr_in serv_addr;
    serv_addr.sin_family = AF_INET;
    serv_addr.sin_port = htons(PORT);

    if(inet_pton(AF_INET, SERVER_IP, &serv_addr.sin_addr)<=0) {
        cout << "Invalid address\n";
        return 1;
    }

    // Conectare la server
    if(connect(sock,(struct sockaddr*)&serv_addr,sizeof(serv_addr))<0) {
        perror("connect");
        return 1;
    }

    // Login
    string key;
    cout << "Enter login key: ";
    cin >> key;
    string loginMsg = "LOGIN " + key;
    send(sock, loginMsg.c_str(), loginMsg.size(), 0);

    char rbuf[256];
    string curr;
    while (true) {
        int n = recv(sock, rbuf, sizeof(rbuf), 0);
        if (n <= 0) {
            std::cout << "Server closed before login response.\n";
            close(sock);
            return 1;
        }
        curr.append(rbuf, n);
        size_t eol = curr.find('\n');
        if (eol != string::npos) {
            string resp = curr.substr(0, eol);
            if (!resp.empty() && resp.back()=='\r') resp.pop_back();

            if (resp.rfind("LOGIN_OK", 0) == 0) {
                std::cout << "Login OK.\n";
                break; // mergem mai departe
            } else {
                std::cout << "Login FAILED: " << resp << "\n";
                close(sock);
                return 1; // oprim aici, nu pornim jocul
            }
        }
    }

    // alegem modul de joc
    string mode;
    cout << "Game mode (input/predict): ";
    cin >> mode;
    bool isInputMode = (mode == "input");
    string startMsg = "START " + mode;
    send(sock, startMsg.c_str(), startMsg.size(), 0);

    // Pornim ncurses
    initscr();
    cbreak();
    noecho();
    keypad(stdscr, TRUE);
    nodelay(stdscr, TRUE);

    // Pornim thread-ul pentru ascultarea serverului
    thread(listenServer, sock).detach();

    // timpul pentru PING/PONG
    lastPong = nowSec();
    nextPing = nowSec() + 1;

    mvprintw(HEIGHT+7, 0, "Use arrow keys to move. Press 'q' to quit.");
    refresh();

    int ch;
    signal(SIGPIPE, SIG_IGN);

    while(state.running) {
        // trimite PING la 1s
        long long now = nowSec();
        if (now >= nextPing) {
            const char* ping = "PING";
            send(sock, ping, 4, 0);
            nextPing = now + 1;
        }
        // verifică timeout-ul de 5s pentru PONG
        if (now - lastPong > 5) {
            mvprintw(HEIGHT+6, 0, "Connection lost (no PONG > 5s).");
            refresh();
            state.running = false;
            break;
        }
        ch = getch();
        if (ch == 'q') break;

        string msg;
        pair<int,int> newDir = state.localDir;

        // transform tasta apăsată în direcție
        if (ch == KEY_UP)    { msg = "INPUT UP";    newDir = { 0,-1}; }
        if (ch == KEY_DOWN)  { msg = "INPUT DOWN";  newDir = { 0, 1}; }
        if (ch == KEY_LEFT)  { msg = "INPUT LEFT";  newDir = {-1, 0}; }
        if (ch == KEY_RIGHT) { msg = "INPUT RIGHT"; newDir = { 1, 0}; }

        if (!msg.empty()) {
            if (isInputMode) {
                // --- MODUL INPUT: trimitem la FIECARE apăsare, chiar dacă direcția nu s-a schimbat
                send(sock, msg.c_str(), msg.size(), 0);

                // prediction: un pas local per apăsare
                {
                    std::lock_guard<std::mutex> lock(state.mtx);
                    if (state.hasServerState && !state.snake.empty()) {
                        auto head = state.snake[0];
                        std::pair<int,int> nh = { head.first + newDir.first,
                                                head.second + newDir.second };
                        state.snake.insert(state.snake.begin(), nh);
                        if (!state.snake.empty()) state.snake.pop_back();
                    }
                }
                state.localDir = newDir;
                drawGame();
            } else {
                // --- MODUL PREDICT: trimitem doar dacă direcția s-a schimbat
                if (newDir != state.localDir) {
                send(sock, msg.c_str(), msg.size(), 0);
                state.localDir = newDir;

                // client prediction: un pas local per schimbare de direcție
                {
                    std::lock_guard<std::mutex> lock(state.mtx);
                    if (state.hasServerState && !state.snake.empty()) {
                        auto head = state.snake[0];
                        std::pair<int,int> nh = { head.first + state.localDir.first,
                                                head.second + state.localDir.second };
                        state.snake.insert(state.snake.begin(), nh);
                        if (!state.snake.empty()) state.snake.pop_back();
                    }
                }
            drawGame();
                }
            }
        }

        this_thread::sleep_for(chrono::milliseconds(50));
    }
    
    mvprintw(HEIGHT+8, 0, "Jocul s-a terminat. Inchidere in 5 secunde...");
    refresh();
    this_thread::sleep_for(std::chrono::seconds(5));
    endwin();
    close(sock);
    return 0;
}
