FROM agent0ai/agent-zero:latest

# install ocean eterna MCP server dependencies
COPY mcp_requirements.txt /tmp/mcp_requirements.txt
RUN /opt/venv-a0/bin/pip install --no-cache-dir -r /tmp/mcp_requirements.txt

# copy OE MCP server + doc processor into container
RUN mkdir -p /opt/ocean-eterna
COPY mcp_server.py /opt/ocean-eterna/mcp_server.py
COPY doc_processor.py /opt/ocean-eterna/doc_processor.py

LABEL agent.name="sloan" \
      agent.description="Agent Zero + Ocean Eterna BM25 search"
