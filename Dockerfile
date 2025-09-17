# Use a lightweight Python image
FROM python:3.11-slim

# Set working directory
WORKDIR /app

# Copy project files
COPY . /app

# Install dependencies
RUN pip install --no-cache-dir -r requirements.txt

# Expose port 2002 for SMTP relay
EXPOSE 2002

# Run the relay
CMD ["python3", "-u", "smtp_relay.py"]
