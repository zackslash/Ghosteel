#ifndef SCROLLENCRYPTOR_H
#define SCROLLENCRYPTOR_H

#include <QObject>
#include <QByteArray>
#include <QVector>

#ifdef SAILFISH_SECRETS
#include <memory>
namespace Sailfish { namespace Secrets { class SecretManager; } }
namespace Sailfish { namespace Crypto { class CryptoManager; class Key; } }
#endif

// Encrypts/decrypts scrollback data using the Sailfish Secrets + Crypto framework.
// The AES-256 key is stored in the daemon and never enters app memory.
//
// If isAvailable() returns false, callers should skip encryption entirely
// rather than falling back to plaintext. Scrollback is silently dropped
// when encryption is unavailable.
//
// Thread safety: main thread only. Uses synchronous waitForFinished() calls.
class ScrollEncryptor : public QObject
{
    Q_OBJECT

public:
    explicit ScrollEncryptor(QObject *parent = nullptr);
    ~ScrollEncryptor();

    // Returns true if encryption subsystem initialized successfully.
    bool isAvailable() const;

    // Encrypt plaintext. Returns binary blob with header, or empty on failure.
    // Caller should fall back to writing plaintext if this returns empty.
    QByteArray encrypt(const QByteArray &plaintext);

    // Decrypt ciphertext (with header). Returns plaintext, or empty on failure.
    QByteArray decrypt(const QByteArray &ciphertextWithHeader);

    // Check if data starts with our encrypted file magic header.
    static bool isEncryptedFormat(const QByteArray &data);

    // PKCS7 padding — public for testing.
    static QByteArray pkcs7Pad(const QByteArray &data);
    static QByteArray pkcs7Unpad(const QByteArray &data);

    // Pre-generate IVs for batch encryption. Call during init to avoid
    // D-Bus round-trips during aboutToQuit. Replenishes if below threshold.
    void replenishIVs();

private:
    bool initializeEncryption();
    bool ensureCollection();
    bool ensureKey();
    QByteArray nextIV();

#ifdef SAILFISH_SECRETS
    std::unique_ptr<Sailfish::Secrets::SecretManager> m_secretManager;
    std::unique_ptr<Sailfish::Crypto::CryptoManager> m_cryptoManager;
    Sailfish::Crypto::Key *m_keyReference = nullptr;
#endif

    bool m_available = false;
    bool m_initialized = false;

    // Pre-generated IV pool (16 bytes each for AES-CBC)
    QVector<QByteArray> m_ivPool;
    static const int IV_POOL_TARGET = 32;
    static const int IV_POOL_THRESHOLD = 8;

    // File format: magic(4) + reserved(4) + iv(16) + ciphertext
    static const int HEADER_SIZE = 24;
};

#endif // SCROLLENCRYPTOR_H
