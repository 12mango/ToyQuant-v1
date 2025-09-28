#!/usr/bin/env python3
import csv
import random
import time

# ===================== 可配置参数 =====================
OUT_FILE = "data/synthetic_ticks.csv"  # 输出路径
NUM_TICKS = 200                        # 行情条数
SYMBOL = "EURUSD"
BASE_PRICE = 1.183                     # 基准价格
PRICE_VOLATILITY = 0.0005              # 价格波动范围（±）
BUY_PROB = 0.5                         # 买卖方向概率（0~1，0.5=买卖均等）
SIZE_MIN = 10                          # 最小成交量
SIZE_MAX = 200                         # 最大成交量
TS_START = int(time.time() * 1000)     # 起始时间戳(ms)
TS_STEP = 100                          # 每条tick的时间间隔(ms)

# ===================== 数据生成 =====================
rows = []
for i in range(NUM_TICKS):
    ts = TS_START + i * TS_STEP
    side = "B" if random.random() < BUY_PROB else "S"
    # 价格围绕 BASE_PRICE 上下波动
    price = BASE_PRICE + random.uniform(-PRICE_VOLATILITY, PRICE_VOLATILITY)
    size = random.randint(SIZE_MIN, SIZE_MAX)
    rows.append([ts, SYMBOL, round(price, 5), size, side])

# ===================== 写入文件 =====================
with open(OUT_FILE, "w", newline="") as f:
    writer = csv.writer(f)
    writer.writerow(["ts", "symbol", "price", "size", "side"])
    writer.writerows(rows)

print(f"✅ Generated {len(rows)} ticks -> {OUT_FILE}")
print(f"Example: ts={rows[0][0]}, price={rows[0][2]}, side={rows[0][4]}")
