/*
 * Inteligência Artificial - Trabalho 1 
 * Sistema de roteamento na malha viária de Ribeirão Preto (dados do OpenStreetMap)
 *
 * Uso:
 *   ./roteamento <osmid_origem> <osmid_destino> [peso_desafio2] [--comparar]
 *   (sem argumentos, o programa pergunta a origem e o destino no terminal)
 *
 * ESTRATÉGIA
 *   Uma única função de busca (buscarRota) implementa a família "melhor primeiro":
 *       f(n) = g(n) + peso * h(n)
 *   onde g(n) é a distância já percorrida (metros) e h(n) é a estimativa até o destino.
 *     - peso = 0     -> Dijkstra (sem heurística), usado só como referência (Foi o Primeiro método utilizado no trabalho)
 *     - peso = 1     -> A*, ÓTIMO se h for admissível (Desafio 1)
 *     - peso = w > 1 -> A* ponderado (Weighted A*), subótimo porém garantidamente
 *                       com custo <= w * custo_ótimo (Desafio 2)
 *
 * HEURÍSTICA (distância em metros a partir de latitude/longitude em graus)
 *   h(n) = fator * Haversine(n, destino), com raio da Terra R = 6.371.008,8 m.
 *   - Haversine é o comprimento do arco de círculo máximo entre dois pontos da esfera:
 *       a = sin²(Δφ/2) + cos φ1 · cos φ2 · sin²(Δλ/2)
 *       d = 2R · atan2(√a, √(1−a))
 *     (φ = latitude e λ = longitude, ambas em radianos).
 *   - Admissibilidade: nenhum caminho pelas ruas é menor que a linha reta, pela desigualdade
 *     triangular, então Haversine nunca superestima a distância restante.
 *   - "fator" (<= 1) protege contra pequenas diferenças de arredondamento entre as distâncias
 *     do CSV e o Haversine. Ele é calculado nos próprios dados como o menor valor de
 *     distancia_da_aresta / haversine_da_aresta (limitado a 1). Assim, para toda aresta
 *     (u,v):  h(u) - h(v) <= fator * haversine(u,v) <= distancia(u,v).
 *     Isso é a CONSISTÊNCIA da heurística: cada nó é expandido no máximo uma vez
 *     (sem reabertura), e o primeiro caminho que chega ao destino é ótimo (peso = 1).
 */

#include <algorithm>
#include <cmath>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <limits>
#include <queue>
#include <sstream>
#include <string>
#include <unordered_map>
#include <vector>

using namespace std;

// ============================================================================
// 1. ESTRUTURAS DE DADOS
// ============================================================================

// Trecho de rua que sai de um cruzamento (lista de adjacência).
struct Aresta {
    int destino;        // índice do nó vizinho em Grafo::nos
    double distancia;   // custo do trecho, em metros
    string nome_rua;
};

struct No {
    long long osmid;
    double latitude;    // graus (-90 a +90)
    double longitude;   // graus (-180 a +180)
    vector<Aresta> vizinhos;
};

// Grafo não orientado em memória. Os osmids (números enormes e esparsos) são convertidos
// para índices 0..N-1 uma única vez na leitura; a busca usa vetores indexados, bem mais
// rápidos que mapas hash.
struct Grafo {
    vector<No> nos;
    unordered_map<long long, int> indice;  // osmid -> posição em 'nos'
    double fator_heuristica = 1.0;         // ver comentário de HEURÍSTICA no topo
    size_t qtd_arestas = 0;
};

// Elemento da Fronteira (fila de prioridade).
struct ElementoFronteira {
    int no;        // índice do nó
    double f;      // prioridade: g + peso * h
    double g;      // custo acumulado quando o elemento foi inserido
};

// Ordem da fila: menor f primeiro. No empate de f, vence o maior g (nó mais perto do destino),
// o que reduz expansões. O std::priority_queue é um heap de máximo, então "menor prioridade"
// = f maior ou (f igual e g menor).
struct MenorPrioridade {
    bool operator()(const ElementoFronteira& a, const ElementoFronteira& b) const {
        if (a.f != b.f) return a.f > b.f;
        return a.g < b.g;
    }
};

struct ResultadoBusca {
    bool encontrou = false;
    string algoritmo;
    vector<long long> caminho;  // osmids da origem ao destino
    double distancia = 0.0;     // soma das arestas, em metros
    long long expandidos = 0;   // nós expandidos (definição formal do PDF)
};

// ============================================================================
// 2. FUNÇÕES AUXILIARES
// ============================================================================

// Distância (metros) entre dois pontos dados em graus, pela fórmula de Haversine.
double distanciaHaversine(double lat1, double lon1, double lat2, double lon2) {
    const double RAIO_TERRA_M = 6371008.8;
    const double PI = acos(-1.0);
    double phi1 = lat1 * PI / 180.0;
    double phi2 = lat2 * PI / 180.0;
    double delta_phi = (lat2 - lat1) * PI / 180.0;
    double delta_lambda = (lon2 - lon1) * PI / 180.0;

    double a = sin(delta_phi / 2) * sin(delta_phi / 2) +
               cos(phi1) * cos(phi2) * sin(delta_lambda / 2) * sin(delta_lambda / 2);
    return 2.0 * RAIO_TERRA_M * atan2(sqrt(a), sqrt(1.0 - a));
}

// Divide uma linha CSV em campos, respeitando aspas (ex.: "Rua A, Centro" é um campo só).
vector<string> dividirLinhaCSV(const string& linha) {
    vector<string> campos;
    string campo;
    bool dentro_aspas = false;
    for (char c : linha) {
        if (c == '"') {
            dentro_aspas = !dentro_aspas;
        } else if (c == ',' && !dentro_aspas) {
            campos.push_back(campo);
            campo.clear();
        } else if (c != '\r') {  // ignora o \r de arquivos com final de linha do Windows
            campo.push_back(c);
        }
    }
    campos.push_back(campo);
    return campos;
}

// Procura a coluna pelo nome no cabeçalho; se não achar, usa a posição padrão do PDF.
int acharColuna(const vector<string>& cabecalho, const string& nome, int posicao_padrao) {
    for (size_t i = 0; i < cabecalho.size(); i++) {
        string limpo;
        for (char c : cabecalho[i])
            if (isalnum((unsigned char)c) || c == '_') limpo.push_back((char)tolower(c));
        if (limpo == nome) return (int)i;
    }
    return posicao_padrao;
}

// ============================================================================
// 3. LEITURA DOS ARQUIVOS .csv
// ============================================================================

bool carregarNos(const string& nomeArquivo, Grafo& grafo) {
    ifstream arquivo(nomeArquivo);
    if (!arquivo.is_open()) {
        cerr << "Erro: nao foi possivel abrir o arquivo " << nomeArquivo << endl;
        return false;
    }

    string linha;
    getline(arquivo, linha);  // cabeçalho
    vector<string> cab = dividirLinhaCSV(linha);
    int col_osmid = acharColuna(cab, "osmid", 0);
    int col_lat = acharColuna(cab, "latitude", 1);
    int col_lon = acharColuna(cab, "longitude", 2);
    int col_max = max(col_osmid, max(col_lat, col_lon));

    int linhas_invalidas = 0;
    while (getline(arquivo, linha)) {
        if (linha.empty() || linha == "\r") continue;
        vector<string> campos = dividirLinhaCSV(linha);
        try {
            if ((int)campos.size() <= col_max) throw invalid_argument("linha curta");
            No no;
            no.osmid = stoll(campos[col_osmid]);
            no.latitude = stod(campos[col_lat]);
            no.longitude = stod(campos[col_lon]);
            if (grafo.indice.count(no.osmid)) continue;  // osmid repetido: mantém o primeiro
            grafo.indice[no.osmid] = (int)grafo.nos.size();
            grafo.nos.push_back(no);
        } catch (...) {
            linhas_invalidas++;
        }
    }
    if (linhas_invalidas > 0)
        cerr << "Aviso: " << linhas_invalidas << " linha(s) invalida(s) ignorada(s) em " << nomeArquivo << endl;
    return !grafo.nos.empty();
}

bool carregarArestas(const string& nomeArquivo, Grafo& grafo) {
    ifstream arquivo(nomeArquivo);
    if (!arquivo.is_open()) {
        cerr << "Erro: nao foi possivel abrir o arquivo " << nomeArquivo << endl;
        return false;
    }

    string linha;
    getline(arquivo, linha);  // cabeçalho
    vector<string> cab = dividirLinhaCSV(linha);
    int col_origem = acharColuna(cab, "origem", 0);
    int col_destino = acharColuna(cab, "destino", 1);
    int col_dist = acharColuna(cab, "distancia", 2);
    int col_nome = acharColuna(cab, "nome_rua", 3);
    int col_max = max(col_origem, max(col_destino, col_dist));

    int linhas_invalidas = 0, nos_desconhecidos = 0;
    while (getline(arquivo, linha)) {
        if (linha.empty() || linha == "\r") continue;
        vector<string> campos = dividirLinhaCSV(linha);
        try {
            if ((int)campos.size() <= col_max) throw invalid_argument("linha curta");
            long long osmid_origem = stoll(campos[col_origem]);
            long long osmid_destino = stoll(campos[col_destino]);
            double distancia = stod(campos[col_dist]);
            if (!(distancia >= 0.0) || isinf(distancia)) throw invalid_argument("distancia invalida");
            string nome_rua = ((int)campos.size() > col_nome) ? campos[col_nome] : "";

            auto it_o = grafo.indice.find(osmid_origem);
            auto it_d = grafo.indice.find(osmid_destino);
            if (it_o == grafo.indice.end() || it_d == grafo.indice.end()) {
                nos_desconhecidos++;  // aresta aponta para nó que não está em nos.csv
                continue;
            }
            int o = it_o->second, d = it_d->second;

            // Grafo NÃO orientado: cada trecho vale nos dois sentidos.
            grafo.nos[o].vizinhos.push_back({d, distancia, nome_rua});
            grafo.nos[d].vizinhos.push_back({o, distancia, nome_rua});
            grafo.qtd_arestas++;
        } catch (...) {
            linhas_invalidas++;
        }
    }
    if (linhas_invalidas > 0)
        cerr << "Aviso: " << linhas_invalidas << " linha(s) invalida(s) ignorada(s) em " << nomeArquivo << endl;
    if (nos_desconhecidos > 0)
        cerr << "Aviso: " << nos_desconhecidos << " aresta(s) com osmid ausente em nos.csv foram ignoradas" << endl;
    return true;
}

// Calcula o fator que mantém a heurística admissível e consistente (ver topo do arquivo).
// Arestas com menos de 1 m em linha reta são ignoradas: a razão distancia/haversine fica
// instável (e irrelevante) para trechos minúsculos.
void calcularFatorHeuristica(Grafo& grafo) {
    double menor_razao = 1.0;
    for (const No& u : grafo.nos) {
        for (const Aresta& a : u.vizinhos) {
            const No& v = grafo.nos[a.destino];
            double reta = distanciaHaversine(u.latitude, u.longitude, v.latitude, v.longitude);
            if (reta >= 1.0) menor_razao = min(menor_razao, a.distancia / reta);
        }
    }
    grafo.fator_heuristica = menor_razao;
}

// ============================================================================
// 4. ALGORITMO DE BUSCA
// ============================================================================

// Reconstrói a rota (osmids) seguindo os pais do destino até a origem.
vector<long long> reconstruirCaminho(const Grafo& grafo, const vector<int>& pai, int origem, int destino) {
    vector<long long> caminho;
    for (int passo = destino; passo != -1; passo = pai[passo]) {
        caminho.push_back(grafo.nos[passo].osmid);
        if (passo == origem) break;
    }
    reverse(caminho.begin(), caminho.end());
    return caminho;
}

// Busca "melhor primeiro" com f(n) = g(n) + peso * h(n).
// Fronteira: fila de prioridade (heap binário), pois a cada passo precisamos do nó de menor f.
// Custo por operação O(log |Fronteira|); a fila usa "remoção preguiçosa": quando um nó recebe
// um custo melhor, insere-se uma nova entrada e a antiga é descartada ao ser retirada.
ResultadoBusca buscarRota(const Grafo& grafo, long long osmid_origem, long long osmid_destino,
                          double peso, const string& nome_algoritmo) {
    ResultadoBusca resultado;
    resultado.algoritmo = nome_algoritmo;

    int origem = grafo.indice.at(osmid_origem);
    int destino = grafo.indice.at(osmid_destino);
    const No& no_destino = grafo.nos[destino];

    // h(n): estimativa (metros) do restante do caminho. peso = 0 anula a heurística (Dijkstra).
    auto heuristica = [&](int n) {
        if (peso == 0.0) return 0.0;
        const No& no = grafo.nos[n];
        return grafo.fator_heuristica *
               distanciaHaversine(no.latitude, no.longitude, no_destino.latitude, no_destino.longitude);
    };

    const double INFINITO = numeric_limits<double>::infinity();
    vector<double> custo(grafo.nos.size(), INFINITO);  // g(n): melhor custo conhecido
    vector<int> pai(grafo.nos.size(), -1);
    vector<char> expandido(grafo.nos.size(), 0);       // nó já retirado e expandido

    priority_queue<ElementoFronteira, vector<ElementoFronteira>, MenorPrioridade> fronteira;
    custo[origem] = 0.0;
    fronteira.push({origem, peso * heuristica(origem), 0.0});

    while (!fronteira.empty()) {
        ElementoFronteira atual = fronteira.top();
        fronteira.pop();

        // Entrada obsoleta: o nó já foi expandido por um caminho melhor. Descartar não é expansão.
        if (expandido[atual.no]) continue;

        if (atual.no == destino) {
            resultado.encontrou = true;
            resultado.distancia = custo[destino];
            resultado.caminho = reconstruirCaminho(grafo, pai, origem, destino);
            return resultado;
        }

        // CONTAGEM DE NÓS EXPANDIDOS: este é o ÚNICO ponto do código que incrementa o contador.
        // Um nó é expandido no momento em que sai da Fronteira e vira o "nó atual", antes de
        // seus vizinhos serem avaliados. Nós descartados como obsoletos (linha acima) e o
        // destino não passam por aqui.(Utilizado LLM para verificar a contagem de expanções)
        expandido[atual.no] = 1;
        resultado.expandidos++;

        for (const Aresta& rua : grafo.nos[atual.no].vizinhos) {
            int vizinho = rua.destino;
            if (expandido[vizinho]) continue;  // heurística consistente: custo já é final
            double novo_custo = custo[atual.no] + rua.distancia;
            if (novo_custo < custo[vizinho]) {
                custo[vizinho] = novo_custo;
                pai[vizinho] = atual.no;
                fronteira.push({vizinho, novo_custo + peso * heuristica(vizinho), novo_custo});
            }
        }
    }
    return resultado;  // fronteira esgotada: não há rota entre origem e destino
}

// ============================================================================
// 5. SAÍDAS (template do enunciado)
// ============================================================================

void imprimirSeparador() {
    cout << "# ------------------------------------------\n";
}

void imprimirCabecalho(const string& titulo, const string& algoritmo, long long origem, long long destino) {
    cout << "\n";
    imprimirSeparador();
    cout << "# RESULTADO DA BUSCA - " << titulo << "\n";
    imprimirSeparador();
    cout << "# Algoritmo utilizado: " << algoritmo << "\n";
    cout << "# Origem (osmid): " << origem << "\n";
    cout << "# Destino (osmid): " << destino << "\n";
    imprimirSeparador();
}

void imprimirDesafio1(const ResultadoBusca& r, long long origem, long long destino) {
    imprimirCabecalho("DESAFIO 1 (ROTA ÓTIMA)", r.algoritmo, origem, destino);
    cout << "# Caminho encontrado (Qtd nós): " << r.caminho.size() << " cruzamentos\n";
    cout << "# Distância da Rota: " << fixed << setprecision(2) << r.distancia << " metros\n";
    cout << "# NÓS EXPANDIDOS: " << r.expandidos << "\n";
    imprimirSeparador();
}

// Variação percentual de 'valor' em relação a 'base', com sinal explícito (+12.13 / -75.20).
string formatarVariacao(double valor, double base) {
    double pct = (base > 0.0) ? (valor - base) / base * 100.0 : 0.0;
    ostringstream saida;
    saida << showpos << fixed << setprecision(2) << pct << "%";
    return saida.str();
}

// Desafio 2: as porcentagens são calculadas em relação ao resultado ótimo do Desafio 1.
void imprimirDesafio2(const ResultadoBusca& r, const ResultadoBusca& base, long long origem, long long destino) {
    imprimirCabecalho("DESAFIO 2 (ROTA EXPRESSA)", r.algoritmo, origem, destino);
    cout << "# Caminho encontrado (Qtd nós): " << r.caminho.size() << " cruzamentos\n";
    cout << "# Distância da Rota: " << fixed << setprecision(2) << r.distancia << " metros ("
         << formatarVariacao(r.distancia, base.distancia) << ")\n";
    cout << "# NÓS EXPANDIDOS: " << r.expandidos << " ("
         << formatarVariacao((double)r.expandidos, (double)base.expandidos) << ")\n";
    imprimirSeparador();

    if (base.distancia > 0.0 && r.distancia > base.distancia * 1.15 + 1e-9)
        cout << "AVISO: a rota expressa passou da tolerância de 15%; reduza o peso do Desafio 2.\n";
}

// Grava um osmid por linha (formato esperado pelo verRota.py adaptado).
void salvarRota(const string& nomeArquivo, const vector<long long>& caminho) {
    ofstream saida(nomeArquivo);
    if (!saida.is_open()) {
        cerr << "Erro: nao foi possivel criar o arquivo " << nomeArquivo << endl;
        return;
    }
    for (long long osmid : caminho) saida << osmid << "\n";
    cout << "Arquivo '" << nomeArquivo << "' gerado com sucesso!\n";
}

// ============================================================================
// 6. PROGRAMA PRINCIPAL
// ============================================================================

int main(int argc, char* argv[]) {
    // Peso padrão do Desafio 2. Com h consistente, o A* ponderado garante custo <= peso * ótimo,
    // então 1.15 respeita a tolerância de 15%l.
    double peso_desafio2 = 1.15;
    bool comparar = false;
    vector<string> parametros;
    for (int i = 1; i < argc; i++) {
        string argumento = argv[i];
        if (argumento == "--comparar") comparar = true;
        else parametros.push_back(argumento);
    }

    long long osmid_origem = 0, osmid_destino = 0;
    try {
        if (parametros.size() >= 2) {
            osmid_origem = stoll(parametros[0]);
            osmid_destino = stoll(parametros[1]);
        } else {
            cout << "Informe o osmid de origem: ";
            cin >> osmid_origem;
            cout << "Informe o osmid de destino: ";
            cin >> osmid_destino;
            if (!cin) throw invalid_argument("entrada invalida");
        }
        if (parametros.size() >= 3) peso_desafio2 = stod(parametros[2]);
    } catch (...) {
        cerr << "Uso: " << argv[0] << " <osmid_origem> <osmid_destino> [peso_desafio2] [--comparar]" << endl;
        return 1;
    }
    if (peso_desafio2 < 1.0) {
        cerr << "Erro: o peso do Desafio 2 deve ser >= 1.0" << endl;
        return 1;
    }

    Grafo grafo;
    if (!carregarNos("nos.csv", grafo) || !carregarArestas("arestas.csv", grafo)) {
        cerr << "Erro: falha ao carregar a base de dados." << endl;
        return 1;
    }
    calcularFatorHeuristica(grafo);
    cout << "Grafo carregado: " << grafo.nos.size() << " cruzamentos e " << grafo.qtd_arestas << " trechos.\n";

    if (!grafo.indice.count(osmid_origem)) {
        cerr << "Erro: o osmid de origem " << osmid_origem << " nao existe em nos.csv." << endl;
        return 1;
    }
    if (!grafo.indice.count(osmid_destino)) {
        cerr << "Erro: o osmid de destino " << osmid_destino << " nao existe em nos.csv." << endl;
        return 1;
    }

    // Desafio 1: A* com peso 1 (heurística consistente) -> rota de distância mínima.
    ResultadoBusca otima = buscarRota(grafo, osmid_origem, osmid_destino, 1.0, "A* (heurística de Haversine)");
    if (!otima.encontrou) {
        cout << "\nNenhuma rota encontrada entre " << osmid_origem << " e " << osmid_destino
             << ": os pontos estao em regioes desconectadas do mapa." << endl;
        return 0;
    }
    imprimirDesafio1(otima, osmid_origem, osmid_destino);
    salvarRota("rota_otima.txt", otima.caminho);

    // Desafio 2: A* ponderado -> troca otimalidade por menos expansões (limite: peso * ótimo).
    ostringstream nome2;
    nome2 << "A* ponderado (Haversine, peso " << fixed << setprecision(2) << peso_desafio2 << ")";
    ResultadoBusca expressa = buscarRota(grafo, osmid_origem, osmid_destino, peso_desafio2, nome2.str());
    imprimirDesafio2(expressa, otima, osmid_origem, osmid_destino);
    salvarRota("rota_expressa.txt", expressa.caminho);

    return 0;
}
