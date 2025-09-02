#include <bits/stdc++.h>
#include <arpa/inet.h>
#include <unistd.h>

#define PORT 65432
#define WIDTH 20
#define HEIGHT 15
#define SECRET_KEY "1234"

using namespace std;

// Informații despre un client conectat
struct Client {
    int sock;
    string mode; // "input" sau "predict"
    bool logged = false;
};

// Starea jocului pentru fiecare client
struct GameSession {
    Client client;
    // vector de poziții (x,y) pentru fiecare segment al șarpelui
    vector<pair<int,int>> snake;

    pair<int,int> direction{1,0}; // dreapta
    // poziția fructului
    pair<int,int> fruit;
    int score = 0;
    bool running = true;
    // 
    pair<int,int> next_input{0,0};
};


mutex mtx;
// Lista tuturor sesiunilor de joc active
vector<GameSession*> sessions;

// Generează o poziție aleatoare pentru fruct, care să nu fie pe șarpe
pair<int,int> spawnFruit(const vector<pair<int,int>>& snake) {
    pair<int,int> point_fruit;
    do {
        point_fruit = {rand()%WIDTH, rand()%HEIGHT};
    } while(find(snake.begin(), snake.end(), point_fruit) != snake.end());
    return point_fruit;
}

// Trimite un mesaj către client
void sendMsg(int sock, const string& msg) {
    string out = msg + "\n";
    send(sock, out.c_str(), out.size(), 0);
}

// Verifică dacă există input de la client
bool hasInput(const pair<int,int>& p) {
    return !(p.first == 0 && p.second == 0);
}

// Bucla principală a jocului pentru fiecare sesiune
void gameLoop(GameSession* session) {
    using clock = chrono::steady_clock;
    using ns    = chrono::nanoseconds;
    using ms    = chrono::milliseconds;

    const ns STEP = ns(33'333'333);   // FIXED 30 FPS
    const ms MOVE_INTERVAL(220);
    auto next = clock::now();
    ns move_accum(0);

    while (session->running) {
        // programăm exact următorul frame
        next += STEP;
        this_thread::sleep_until(next);
        move_accum += STEP;

        lock_guard<mutex> lock(mtx);

        // procesăm input-ul clientului
        bool has_input = hasInput(session->next_input);
        if (has_input) {
            // actualizează direcția curentă
            session->direction = session->next_input;
            session->next_input = {0,0};
        }

        // decid 
        bool should_move = false;

        if (session->client.mode == "input") {
            // doar dacă a sosit input acum
            should_move = has_input;
        } else { // "predict"
            if (move_accum >= MOVE_INTERVAL) {
                move_accum -= MOVE_INTERVAL;
                // continuă să se miște constant cu direcția curentă
                should_move = true;
            }
        }

        if (!should_move) {
            // nimic de făcut în acest frame; nu trimitem UPDATE
            continue;
        }

        // aplică mutarea șarpelui
        // noua poziție a capului șarpelui
        auto head = session->snake[0];

        int new_x = head.first + session->direction.first;
        int new_y = head.second + session->direction.second;

        pair<int,int> new_head{new_x, new_y};

        // coliziune cu peretele daca iese din grilă
        if (new_head.first < 0 || new_head.second < 0 ||
            new_head.first >= WIDTH || new_head.second >= HEIGHT) {
            // mesaj de disconnect trimis de server catre client
            sendMsg(session->client.sock, "DISCONNECT reason=lose");
            session->running = false;
            break;
        }

        // inserează noul cap și elimină coada
        session->snake.insert(session->snake.begin(), new_head);
        session->snake.pop_back();

        //  daca a  ajuns la fruct il consuma si creste scorul
        if (new_head == session->fruit) {
            session->score += 10;

            session->fruit = spawnFruit(session->snake);
        }

        // construim mesajul UPDATE cu noua stare a fructului și șarpelui
        // format: "UPDATE score fruit_x fruit_y seg1_x seg1_y seg2_x seg2_y ..."
        string message;
        message = "UPDATE " + to_string(session->score) +
                " " + to_string(session->fruit.first) + " " + to_string(session->fruit.second);
        for (auto &seg : session->snake) {
            message += " " + to_string(seg.first) + " " + to_string(seg.second);
        }
        sendMsg(session->client.sock, message);

    }
    // sesiunea s-a terminat
    close(session->client.sock);
}

// functia ruleaza pe un thread separat pentru fiecare client
void handleClient(int sock) {
    char buffer[1024];
    GameSession* session = nullptr;

    while(true) {
        memset(buffer, 0, sizeof(buffer));
        // citim un mesaj de la client
        int valread = read(sock, buffer, 1024);
        if(valread <= 0) break;

        string msg(buffer);
        msg.erase(msg.find_last_not_of("\n\r")+1);

        // dacă nu există o sesiune activă pentru acest client, așteptăm comanda LOGIN sau START
        if(!session) {
            // daca mesajul este LOGIN verificam cheia
            // daca cheia este corecta trimitem LOGIN_OK altfel LOGIN_FAIL si inchidem conexiunea
            if(msg.substr(0,5) == "LOGIN") {
                string key = msg.substr(6);
                if(key == SECRET_KEY) {
                    sendMsg(sock, "LOGIN_OK");
                } else {
                    sendMsg(sock, "LOGIN_FAIL");
                    close(sock);
                    return;
                }
            }
            // daca mesajul este START cream o noua sesiune de joc si pornim bucla jocului pe un thread separat
            else if(msg.substr(0,5) == "START") {
                string mode = msg.substr(6);
                session = new GameSession();
                session->client = {sock, mode, true};
                session->snake = {{5,5},{4,5},{3,5},{2,5}};
                session->fruit = spawnFruit(session->snake);
                {
                    lock_guard<mutex> lock(mtx);
                    sessions.push_back(session);
                }
                thread(gameLoop, session).detach();
            }
            // daca avem o sesiune extragem input-ul
            // actualizam directia umatoarea in functie de input
        } else {
            if(msg.substr(0,5) == "INPUT") {
                string d = msg.substr(6);
                if(d=="UP") session->next_input = {0,-1};
                else if(d=="DOWN") session->next_input = {0,1};
                else if(d=="LEFT") session->next_input = {-1,0};
                else if(d=="RIGHT") session->next_input = {1,0};
                // raspunde cu PONG la PING ca sa verifice ca serverul e activ
            } else if (msg == "PING") {
                sendMsg(sock,"PONG");
            }
        } 
    }
}

int main() {
    srand(time(0));
    // creăm socket-ul server
    int server_fd = socket(AF_INET, SOCK_STREAM, 0);
    if(server_fd == 0) {
        perror("socket failed");
        exit(EXIT_FAILURE);
    }

    sockaddr_in address;
    int opt = 1;
    // completez adresa pe care serverul ascultă
    setsockopt(server_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = INADDR_ANY;
    address.sin_port = htons(PORT);

    // legăm socket-ul de adresă și începem să ascultăm conexiuni
    if(bind(server_fd, (struct sockaddr*)&address, sizeof(address))<0) {
        perror("bind failed");
        exit(EXIT_FAILURE);
    }
    if(listen(server_fd, 3)<0) {
        perror("listen");
        exit(EXIT_FAILURE);
    }

    cout << "Server started on port " << PORT << endl;

    // bucla principală de acceptare a conexiunilor
    while(true) {
        socklen_t addrlen = sizeof(address);
        // acceptăm o nouă conexiune
        int new_socket = accept(server_fd, (struct sockaddr*)&address, &addrlen);
        if(new_socket<0) {
            perror("accept");
            exit(EXIT_FAILURE);
        }
        // pornim un thread separat pentru a gestiona clientul
        thread(handleClient, new_socket).detach();
    }
}
