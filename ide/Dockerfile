# KTSN LED IDE: build the DSL core to WebAssembly, test and build the web app, serve it with nginx.
# Build context is the repository root (the core lives in components/dsl). Use ide/run.sh, or:
#   docker compose -f ide/docker-compose.yml up --build      -> http://localhost:8080
ARG EMSDK_TAG=4.0.15

FROM emscripten/emsdk:${EMSDK_TAG} AS wasm
WORKDIR /src
COPY components/dsl components/dsl
COPY main/gamma.c main/gamma.h main/dither.h main/wire.c main/wire.h main/astro.c main/astro.h main/
COPY ide/wasm ide/wasm
RUN sh ide/wasm/build.sh

FROM node:22-alpine AS web
WORKDIR /src/ide/web
COPY ide/web/package.json ide/web/package-lock.json ./
RUN npm ci --no-fund --no-audit
COPY ide/web ./
COPY --from=wasm /src/ide/web/src/wasm/generated ./src/wasm/generated
RUN npm test && npm run build

FROM nginx:1.27-alpine
COPY ide/nginx.conf /etc/nginx/conf.d/default.conf
COPY --from=web /src/ide/web/dist /usr/share/nginx/html
EXPOSE 80
