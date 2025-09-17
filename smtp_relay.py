from aiosmtpd.controller import Controller
from email.parser import BytesParser
from email.policy import default
from new import send_email  # Import from your working script
import time

class CustomSMTPHandler:
    async def handle_DATA(self, server, session, envelope):
        print("📨 Received email")
        try:
            # Parse the incoming email content
            message = BytesParser(policy=default).parsebytes(envelope.content)

            # Extract fields
            to_addresses = [addr.strip() for addr in message.get_all('To', [])]
            subject = message.get('Subject', 'No Subject')

            plain_body = None
            html_body = None

            # Handle multipart emails
            if message.is_multipart():
                for part in message.walk():
                    content_type = part.get_content_type()
                    if content_type == 'text/plain':
                        plain_body = part.get_content()
                    elif content_type == 'text/html':
                        html_body = part.get_content()
            else:
                plain_body = message.get_content()

            print(f"👉 To: {to_addresses}")
            print(f"👉 Subject: {subject}")

            # Call your existing send_email function from new.py
            send_email(
                to_addresses=to_addresses,
                subject=subject,
                body_text=plain_body or "No plain text body",
                body_html=html_body
            )

            print("✅ Email sent successfully via Office365")
            return '250 Message accepted for delivery'

        except Exception as e:
            print(f"❌ Failed to process/send email: {e}")
            return f'550 Failed to send email: {e}'


if __name__ == "__main__":
    print("🚀 Starting local SMTP relay on localhost:2002")
    handler = CustomSMTPHandler()
    controller = Controller(handler, hostname='0.0.0.0', port=2002)
    controller.start()

    try:
        while True:
            time.sleep(1)
    except KeyboardInterrupt:
        print("🛑 Stopping SMTP relay...")
        controller.stop()
