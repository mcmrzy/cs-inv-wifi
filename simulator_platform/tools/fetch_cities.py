#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
fetch_cities.py — 从阿里 DataV GeoAtlas 拉取全国地级市/地区坐标与名称

原理：
  主文件 100000_full.json 含省级 features（properties.adcode=省6位码）。
  对每省再拉 https://geo.datav.aliyun.com/areas_v3/bound/{省码}_full.json
  （内含该省所有地级/直辖市级 features，properties.center = 几何中心 [lng,lat]）。
  汇总成地级市表 -> 写入 server/src/geo-cities.json。

用法： python fetch_cities.py
输出： ../server/src/geo-cities.json   [{code,name,province,lat,lng,tzOffset,climate}]
"""
import json, urllib.request, time, random

BASE = "https://geo.datav.aliyun.com/areas_v3/bound/"
OUT = r"D:\CS_INV_WIFI\simulator_platform\server\src\geo-cities.json"

def get(url, tries=3):
    for i in range(tries):
        try:
            req = urllib.request.Request(url, headers={"User-Agent": "Mozilla/5.0"})
            with urllib.request.urlopen(req, timeout=25) as r:
                return json.loads(r.read().decode("utf-8"))
        except Exception as e:
            time.sleep(1.5)
    raise RuntimeError(f"fetch failed: {url}")

def main():
    root = get(BASE + "100000_full.json")
    provs = [f for f in root["features"] if str(f["properties"].get("adcode", "")).endswith("0000")]
    provs = [f for f in provs if str(f["properties"].get("adcode", "")) != "100000"]
    print(f"省份数: {len(provs)}")

    cities = []
    for p in provs:
        adcode = str(p["properties"]["adcode"])
        prov_name = p["properties"]["name"]
        try:
            sub = get(BASE + adcode + "_full.json")
        except RuntimeError as e:
            print("  skip", prov_name, e); continue
        feats = [f for f in sub.get("features", []) if f.get("geometry")]
        for f in feats:
            props = f.get("properties", {})
            center = props.get("center")
            if not center:
                # 从几何取外包框中心兜底
                b = f.get("bbox")
                center = [ (b[0]+b[2])/2, (b[1]+b[3])/2 ] if b else None
            if not center: continue
            code = str(props.get("adcode", ""))
            name = props.get("name", "")
            if not name or not code: continue
            cities.append({
                "code": code,
                "name": name,
                "province": prov_name,
                "lat": round(center[1], 5),
                "lng": round(center[0], 5),
            })
        time.sleep(0.3)
        print(f"  +{prov_name}: {len(feats)}")

    # 去重（按 code）
    seen = {}
    for c in cities:
        seen.setdefault(c["code"], c)
    cities = list(seen.values())
    print(f"\n地级市总数: {len(cities)}")

    # 补充气候基线与时区（用省份或粗粒度启发）
    def choose_climate(prov):
        p = prov
        if "新疆" in p or "青海" in p or "西藏" in p: return "northwest"
        if "黑龙江" in p or "吉林" in p or "辽宁" in p or "内蒙古" in p: return "northeast"
        if "广东" in p or "广西" in p or "海南" in p or "福建" in p or "江西" in p or "湖南" in p: return "south"
        if "四川" in p or "贵州" in p or "云南" in p or "重庆" in p: return "southwest"
        if "河南" in p or "湖北" in p or "安徽" in p: return "central"
        if "上海" in p or "江苏" in p or "浙江" in p: return "east"
        return "north"
    for c in cities:
        c["tzOffset"] = 8
        c["climate"] = choose_climate(c["province"])

    with open(OUT, "w", encoding="utf-8") as fh:
        json.dump(cities, fh, ensure_ascii=False, indent=1)
    print("写入:", OUT)

if __name__ == "__main__":
    main()
