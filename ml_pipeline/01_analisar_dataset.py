import os
import glob
import pandas as pd
import numpy as np
import matplotlib.pyplot as plt
import cv2

from config import DATASET_RAW_DIR, MIN_THROTTLE

def analisar_dataset():
    print("=" * 60)
    print("ANALISANDO DATASET BRUTO — ESP32-CAM")
    print("=" * 60)
    print(f"Diretório: {DATASET_RAW_DIR}")

    session_dirs = sorted(glob.glob(os.path.join(DATASET_RAW_DIR, "session_*")))
    print(f"Sessões encontradas: {len(session_dirs)}")

    if not session_dirs:
        print("Nenhuma sessão encontrada!")
        return

    all_records = []
    total_frames_raw = 0
    missing_images = 0

    for s_idx, s_dir in enumerate(session_dirs):
        session_name = os.path.basename(s_dir)
        csv_path = os.path.join(s_dir, "log.csv")
        if not os.path.exists(csv_path):
            print(f"  [AVISO] {session_name} sem log.csv!")
            continue

        try:
            df = pd.read_csv(csv_path)
            # Limpa possíveis espaços em nomes de colunas
            df.columns = [c.strip() for c in df.columns]

            for _, row in df.iterrows():
                frame_filename = str(row['frame']).strip()
                img_path = os.path.join(s_dir, frame_filename)

                total_frames_raw += 1
                if not os.path.exists(img_path):
                    missing_images += 1
                    continue

                steer = float(row['steer'])
                throttle = float(row['throttle'])

                all_records.append({
                    'session': session_name,
                    'frame': frame_filename,
                    'img_path': img_path,
                    'steer': steer,
                    'throttle': throttle
                })
        except Exception as e:
            print(f"  [ERRO] Falha ao ler {session_name}: {e}")

    df_all = pd.DataFrame(all_records)
    print(f"\nTotal de registros lidos no CSV: {total_frames_raw}")
    print(f"Imagens ausentes / corrompidas: {missing_images}")
    print(f"Imagens válidas encontradas: {len(df_all)}")

    # Filtros
    df_parado = df_all[(df_all['throttle'] == 0) & (df_all['steer'] == 0)]
    df_re = df_all[df_all['throttle'] < 0]
    df_util = df_all[df_all['throttle'] > MIN_THROTTLE]

    print("\n--- DISTRIBUIÇÃO DOS DADOS ---")
    print(f"Frames parados (throttle=0, steer=0) : {len(df_parado)} ({len(df_parado)/len(df_all)*100:.1f}%) -> Descartados")
    print(f"Frames em ré (throttle < 0)           : {len(df_re)} ({len(df_re)/len(df_all)*100:.1f}%) -> Descartados")
    print(f"Frames úteis em movimento para treino : {len(df_util)} ({len(df_util)/len(df_all)*100:.1f}%) -> APROVEITADOS")

    print("\n--- ESTATÍSTICAS DE STEER (FRAMES ÚTEIS) ---")
    print(f"Média: {df_util['steer'].mean():.4f}")
    print(f"Desvio Padrão: {df_util['steer'].std():.4f}")
    print(f"Mínimo: {df_util['steer'].min():.4f}")
    print(f"Máximo: {df_util['steer'].max():.4f}")

    esquerda = len(df_util[df_util['steer'] < -0.05])
    reto = len(df_util[(df_util['steer'] >= -0.05) & (df_util['steer'] <= 0.05)])
    direita = len(df_util[df_util['steer'] > 0.05])

    print(f"\nCurvas para Esquerda (steer < -0.05) : {esquerda} ({esquerda/len(df_util)*100:.1f}%)")
    print(f"Retas centrais (|steer| <= 0.05)    : {reto} ({reto/len(df_util)*100:.1f}%)")
    print(f"Curvas para Direita  (steer > 0.05)  : {direita} ({direita/len(df_util)*100:.1f}%)")
    print(f"* O data augmentation (flip horizontal) duplicará esses frames para {len(df_util)*2} imagens perfeitamente balanceadas!")

    # Plot e salvamento de gráfico
    output_plot_path = os.path.join(os.path.dirname(__file__), "relatorio_dataset.png")

    fig = plt.figure(figsize=(14, 10))

    # Subplot 1: Histograma de Steer
    ax1 = fig.add_subplot(2, 2, 1)
    ax1.hist(df_util['steer'], bins=30, color='royalblue', edgecolor='black', alpha=0.7)
    ax1.set_title("Distribuição do Steer (Ângulo de Esterçamento)")
    ax1.set_xlabel("Steer (-1.0 = Esquerda Max, +1.0 = Direita Max)")
    ax1.set_ylabel("Quantidade de Frames")
    ax1.grid(True, linestyle='--', alpha=0.5)

    # Subplot 2: Throttle vs Steer
    ax2 = fig.add_subplot(2, 2, 2)
    ax2.scatter(df_util['steer'], df_util['throttle'], alpha=0.2, color='darkorange', s=10)
    ax2.set_title("Throttle vs Steer")
    ax2.set_xlabel("Steer")
    ax2.set_ylabel("Throttle")
    ax2.grid(True, linestyle='--', alpha=0.5)

    # Subplot 3 & 4: 8 Amostras aleatórias de imagens com valor de steer
    sample_indices = np.random.choice(len(df_util), size=min(8, len(df_util)), replace=False)
    for i, idx in enumerate(sample_indices):
        row = df_util.iloc[idx]
        img = cv2.imread(row['img_path'])
        if img is not None:
            img_rgb = cv2.cvtColor(img, cv2.COLOR_BGR2RGB)
            ax_img = fig.add_subplot(4, 4, 8 + i + 1)
            ax_img.imshow(img_rgb)
            ax_img.set_title(f"S: {row['steer']:.2f} | T: {row['throttle']:.2f}", fontsize=8)
            ax_img.axis('off')

    plt.tight_layout()
    plt.savefig(output_plot_path, dpi=120)
    print(f"\n[OK] Gráfico salvo em: {output_plot_path}")
    print("=" * 60)

if __name__ == "__main__":
    analisar_dataset()
