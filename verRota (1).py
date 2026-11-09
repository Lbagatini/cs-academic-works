# Inteligência Artificial - Trabalho 1 (2º Semestre/2026)
# Integrantes: [Nome 1] - Nº USP [preencher]; [Nome 2] - Nº USP [preencher]; [Nome 3] - Nº USP [preencher]
#
# Plota no mapa as rotas geradas pelo programa em C++ (roteamento.cpp):
#   rota_otima.txt    -> Desafio 1 (linha vermelha)
#   rota_expressa.txt -> Desafio 2 (linha azul tracejada)
# Uso: python verRota.py   (execute na mesma pasta de nos.csv e das rotas)

import os
import folium
import pandas as pd

# 1. Carrega os nós (latitude e longitude de cada osmid)
nos_df = pd.read_csv("nos.csv", index_col='osmid')


def ler_caminho(nome_arquivo):
    """Lê a lista de osmids (um por linha) gerada pelo programa em C++."""
    if not os.path.exists(nome_arquivo):
        return []
    with open(nome_arquivo) as arquivo:
        return [int(linha) for linha in arquivo if linha.strip()]


def para_coordenadas(caminho_ids):
    """Converte osmids em pares (latitude, longitude)."""
    return [(nos_df.loc[no_id, 'latitude'], nos_df.loc[no_id, 'longitude'])
            for no_id in caminho_ids if no_id in nos_df.index]


rota_otima = para_coordenadas(ler_caminho("rota_otima.txt"))
rota_expressa = para_coordenadas(ler_caminho("rota_expressa.txt"))

if not rota_otima:
    raise SystemExit("rota_otima.txt nao encontrado ou vazio. Rode antes o ./roteamento.")

# 2. Mapa centralizado na origem
mapa = folium.Map(location=rota_otima[0], zoom_start=14)

# 3. Desafio 2 primeiro (azul, tracejada) e Desafio 1 por cima (vermelha),
#    para ver os trechos em que as duas rotas coincidem e onde divergem
if rota_expressa:
    folium.PolyLine(rota_expressa, color='blue', weight=5, opacity=0.8, dash_array='10',
                    tooltip="Desafio 2 - rota expressa").add_to(mapa)
folium.PolyLine(rota_otima, color='red', weight=5, opacity=0.8,
                tooltip="Desafio 1 - rota ótima").add_to(mapa)

# 4. Marcadores de início e fim
folium.Marker(rota_otima[0], popup="Origem", icon=folium.Icon(color="green")).add_to(mapa)
folium.Marker(rota_otima[-1], popup="Destino", icon=folium.Icon(color="red")).add_to(mapa)

# 5. Salvar
mapa.save("rotaBuscaGrupoX.html")
print("Mapa de rota gerado com sucesso!")
