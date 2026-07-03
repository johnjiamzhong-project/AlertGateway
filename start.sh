#!/bin/bash
APP_DIR="$(cd "$(dirname "$0")" && pwd)"
BINARY="$APP_DIR/AlertGateway"
CONFIG="$APP_DIR/config/config.json"
LOG="/tmp/ag.log"
PIDFILE="/tmp/ag.pid"

case "${1:-start}" in
  start)
    if [ -f "$PIDFILE" ] && kill -0 "$(cat "$PIDFILE")" 2>/dev/null; then
      echo "AlertGateway already running (PID $(cat "$PIDFILE"))"
      exit 0
    fi
    cd "$APP_DIR"
    # 启动前开启锁频服务（大核/DDR锁频，禁用cpuidle）
    echo "Starting rockchip-performance service..."
    sudo systemctl start rockchip-performance
    
    nohup "$BINARY" "$CONFIG" >> "$LOG" 2>&1 &
    echo $! > "$PIDFILE"
    echo "Started AlertGateway (PID $!), log: $LOG"
    ;;
  stop)
    if [ -f "$PIDFILE" ]; then
      PID=$(cat "$PIDFILE")
      kill -SIGINT "$PID" 2>/dev/null && echo "Stopped AlertGateway (PID $PID)"
      rm -f "$PIDFILE"
    else
      pkill -SIGINT -f AlertGateway && echo "Stopped" || echo "Not running"
    fi
    # 停止后自动恢复为默认调频模式（省电与降低发热）
    echo "Stopping rockchip-performance service..."
    sudo systemctl stop rockchip-performance
    ;;
  status)
    if [ -f "$PIDFILE" ] && kill -0 "$(cat "$PIDFILE")" 2>/dev/null; then
      echo "Running (PID $(cat "$PIDFILE"))"
    else
      echo "Not running"
    fi
    ;;
  log)
    tail -f "$LOG"
    ;;
  restart)
    "$0" stop
    sleep 1
    "$0" start
    ;;
  *)
    echo "Usage: $0 {start|stop|restart|status|log}"
    exit 1
    ;;
esac
