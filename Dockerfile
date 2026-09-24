# --- Stage 1: Build the C++ application ---
FROM gcc:12 AS builder
WORKDIR /app
COPY main.cpp httplib.h ./
RUN g++ -std=c++17 -O2 -o server main.cpp -lpthread

# --- Stage 2: Create a lightweight runtime image ---
FROM debian:bookworm-slim
WORKDIR /app
# Copy the compiled binary from the builder stage
COPY --from=builder /app/server .
# Copy your static frontend files
COPY index.html style.css app.js ./
# Create the data directory for order persistence
RUN mkdir -p data
EXPOSE 8080
CMD ["./server"]
