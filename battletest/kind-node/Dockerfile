# kind node image patched for sandboxes that deny lowering oom_score_adj.
# Only used when local-up.sh detects the restriction; harmless elsewhere.
ARG BASE=kindest/node:v1.34.0
FROM $BASE
COPY runc-wrapper.sh /usr/local/sbin/runc-oomfix
RUN mv /usr/local/sbin/runc /usr/local/sbin/runc.real \
    && cp /usr/local/sbin/runc-oomfix /usr/local/sbin/runc \
    && chmod +x /usr/local/sbin/runc
