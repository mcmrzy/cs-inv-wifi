# 认领模拟平台镜像
FROM node:20-alpine AS build
WORKDIR /app
COPY server/package.json server/package-lock.json* ./
RUN npm install --omit=dev || (npm install && npm prune --omit=dev)

FROM node:20-alpine
WORKDIR /app
ENV NODE_ENV=production
COPY --from=build /app/node_modules ./node_modules
COPY server/src ./src
COPY web ./web

# 暴露 HTTP 端口
EXPOSE 8080

# 数据目录（挂载 volume 持久化认领/设备/快照）
RUN mkdir -p /app/data
VOLUME ["/app/data"]

CMD ["node", "src/index.js"]
