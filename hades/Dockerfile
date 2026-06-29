# Stage 1: build
FROM node:22-alpine AS builder

RUN corepack enable

WORKDIR /app
# Copy all pnpm config before install so the workspace settings and lockfile
# are consistent. pnpm 9+ requires pnpm-workspace.yaml for allowBuilds (esbuild).
COPY package.json pnpm-lock.yaml pnpm-workspace.yaml ./
RUN pnpm install --frozen-lockfile

COPY . .
RUN pnpm build

# Stage 2: serve
FROM nginx:alpine

COPY --from=builder /app/dist /usr/share/nginx/html
COPY nginx.conf /etc/nginx/conf.d/default.conf

EXPOSE 3000
