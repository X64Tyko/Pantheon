# Stage 1: build
FROM node:22-alpine AS builder

RUN corepack enable

WORKDIR /app

# Build context is the repo root (see docker-hades.yml) so this stage can
# also reach VERSION — hades/ is copied in below flattened to the same /app
# layout as before.
#
# Copy all pnpm config before install so the workspace settings and lockfile
# are consistent. pnpm 9+ requires pnpm-workspace.yaml for allowBuilds (esbuild).
COPY hades/package.json hades/pnpm-lock.yaml hades/pnpm-workspace.yaml ./
RUN pnpm install --frozen-lockfile

COPY VERSION VERSION
COPY hades/ .
RUN pnpm build

# Stage 2: serve
FROM nginx:alpine

COPY --from=builder /app/dist /usr/share/nginx/html
COPY hades/nginx.conf /etc/nginx/conf.d/default.conf

EXPOSE 3000
