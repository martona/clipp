//
//  ShareViewController.swift
//  ClippShareExtension
//

import UIKit
import UniformTypeIdentifiers

final class ShareViewController: UIViewController {
    private let iconView = UIImageView()
    private let titleLabel = UILabel()
    private let detailLabel = UILabel()
    private let doneButton = UIButton(type: .system)
    private let activityIndicator = UIActivityIndicatorView(style: .medium)
    private var didStart = false

    override func viewDidLoad() {
        super.viewDidLoad()

        view.backgroundColor = .systemBackground
        preferredContentSize = CGSize(width: 320, height: 260)

        iconView.image = UIImage(systemName: "square.and.arrow.up")
        iconView.tintColor = .secondaryLabel
        iconView.contentMode = .scaleAspectFit
        iconView.translatesAutoresizingMaskIntoConstraints = false
        NSLayoutConstraint.activate([
            iconView.widthAnchor.constraint(equalToConstant: 44),
            iconView.heightAnchor.constraint(equalToConstant: 44),
        ])

        titleLabel.text = "Preparing share"
        titleLabel.font = .preferredFont(forTextStyle: .headline)
        titleLabel.adjustsFontForContentSizeCategory = true
        titleLabel.textAlignment = .center
        titleLabel.numberOfLines = 0

        detailLabel.text = "Looking for clipboard content Clipp can send."
        detailLabel.font = .preferredFont(forTextStyle: .subheadline)
        detailLabel.adjustsFontForContentSizeCategory = true
        detailLabel.textColor = .secondaryLabel
        detailLabel.textAlignment = .center
        detailLabel.numberOfLines = 0

        activityIndicator.startAnimating()

        var configuration = UIButton.Configuration.filled()
        configuration.title = "Done"
        configuration.cornerStyle = .capsule
        configuration.contentInsets = NSDirectionalEdgeInsets(top: 10, leading: 20, bottom: 10, trailing: 20)
        doneButton.configuration = configuration
        doneButton.addTarget(self, action: #selector(doneTapped), for: .touchUpInside)
        doneButton.isHidden = true

        let stackView = UIStackView(arrangedSubviews: [
            iconView,
            activityIndicator,
            titleLabel,
            detailLabel,
            doneButton,
        ])
        stackView.axis = .vertical
        stackView.alignment = .center
        stackView.spacing = 14
        stackView.translatesAutoresizingMaskIntoConstraints = false

        view.addSubview(stackView)

        NSLayoutConstraint.activate([
            stackView.leadingAnchor.constraint(greaterThanOrEqualTo: view.readableContentGuide.leadingAnchor),
            stackView.trailingAnchor.constraint(lessThanOrEqualTo: view.readableContentGuide.trailingAnchor),
            stackView.centerXAnchor.constraint(equalTo: view.centerXAnchor),
            stackView.centerYAnchor.constraint(equalTo: view.centerYAnchor),
            detailLabel.widthAnchor.constraint(lessThanOrEqualToConstant: 260),
        ])
    }

    override func viewDidAppear(_ animated: Bool) {
        super.viewDidAppear(animated)

        guard !didStart else {
            return
        }

        didStart = true
        Task {
            await sendSharedItems()
        }
    }

    @objc private func doneTapped() {
        extensionContext?.completeRequest(returningItems: [], completionHandler: nil)
    }

    private func sendSharedItems() async {
        let extraction = await extractPayloads()
        guard !extraction.payloads.isEmpty else {
            showResult(
                symbolName: "exclamationmark.circle.fill",
                tintColor: .systemOrange,
                title: "Nothing to send",
                detail: ignoredText(extraction.unsupportedCount, fallback: "No supported clipboard items were found.") ?? "No supported clipboard items were found.",
                feedback: UINotificationFeedbackGenerator.FeedbackType.warning
            )
            return
        }

        await MainActor.run {
            titleLabel.text = "Sending with Clipp"
            detailLabel.text = "Sending to trusted devices nearby."
        }

        do {
            let result = try await Task.detached(priority: .userInitiated) {
                try ShareSenderBridge.send(extraction.payloads)
            }.value

            let itemText = plural(result.sentItemCount, singular: "item", plural: "items")
            let attemptedDeviceCount = result.attemptedDeviceCount
            let failedDeviceNames = result.failedDeviceNames
            let hasFailures = !failedDeviceNames.isEmpty || result.reachedDeviceCount < attemptedDeviceCount
            let deviceText = plural(hasFailures ? attemptedDeviceCount : result.reachedDeviceCount, singular: "device", plural: "devices")
            let sentDetail: String
            if hasFailures {
                sentDetail = "Sent \(result.sentItemCount) \(itemText) to \(result.reachedDeviceCount) of \(attemptedDeviceCount) \(deviceText)."
            } else {
                sentDetail = "Sent \(result.sentItemCount) \(itemText) to \(result.reachedDeviceCount) \(deviceText)."
            }
            let failureDetail: String?
            if !failedDeviceNames.isEmpty {
                failureDetail = "Failed: \(failedDeviceNames.joined(separator: ", "))."
            } else if hasFailures {
                let failedCount = attemptedDeviceCount - result.reachedDeviceCount
                let failedDeviceText = plural(failedCount, singular: "device", plural: "devices")
                failureDetail = "\(failedCount) \(failedDeviceText) failed."
            } else {
                failureDetail = nil
            }
            let ignored = ignoredText(extraction.unsupportedCount, fallback: nil)
            let detail = ([ sentDetail, failureDetail, ignored ]
                .compactMap { $0 })
                .joined(separator: " ")

            showResult(
                symbolName: hasFailures ? "exclamationmark.triangle.fill" : "checkmark.circle.fill",
                tintColor: hasFailures ? .systemOrange : .systemGreen,
                title: hasFailures ? "Partially Shared" : "Shared with Clipp",
                detail: detail,
                feedback: hasFailures ? UINotificationFeedbackGenerator.FeedbackType.warning : UINotificationFeedbackGenerator.FeedbackType.success
            )
        } catch {
            let ignored = ignoredText(extraction.unsupportedCount, fallback: nil)
            let detail = ([ error.localizedDescription, ignored ]
                .compactMap { $0 })
                .joined(separator: " ")

            showResult(
                symbolName: "xmark.circle.fill",
                tintColor: .systemRed,
                title: "Unable to send",
                detail: detail,
                feedback: UINotificationFeedbackGenerator.FeedbackType.error
            )
        }
    }

    private func showResult(
        symbolName: String,
        tintColor: UIColor,
        title: String,
        detail: String,
        feedback: UINotificationFeedbackGenerator.FeedbackType
    ) {
        activityIndicator.stopAnimating()
        activityIndicator.isHidden = true
        iconView.image = UIImage(systemName: symbolName)
        iconView.tintColor = tintColor
        titleLabel.text = title
        detailLabel.text = detail
        doneButton.isHidden = false
        UINotificationFeedbackGenerator().notificationOccurred(feedback)
    }

    private func extractPayloads() async -> (payloads: [SharePayload], unsupportedCount: Int) {
        let items = extensionContext?.inputItems as? [NSExtensionItem] ?? []
        var payloads: [SharePayload] = []
        var unsupportedCount = 0

        for item in items {
            for provider in item.attachments ?? [] {
                if let payload = await payload(from: provider) {
                    payloads.append(payload)
                } else {
                    unsupportedCount += 1
                }
            }
        }

        return (payloads, unsupportedCount)
    }

    private func payload(from provider: NSItemProvider) async -> SharePayload? {
        if provider.hasItemConformingToTypeIdentifier(UTType.jpeg.identifier),
           let data = try? await loadImageData(from: provider, type: UTType.jpeg),
           !data.isEmpty {
            return SharePayload.jpegData(data)
        }

        if provider.hasItemConformingToTypeIdentifier(UTType.png.identifier),
           let data = try? await loadImageData(from: provider, type: UTType.png),
           !data.isEmpty {
            return SharePayload.pngData(data)
        }

        if provider.hasItemConformingToTypeIdentifier(UTType.fileURL.identifier),
           let fileURL = try? await loadURL(from: provider, type: UTType.fileURL) {
            let pathExtension = fileURL.pathExtension.lowercased()
            if (pathExtension == "jpg" || pathExtension == "jpeg"),
               let data = try? Data(contentsOf: fileURL),
               !data.isEmpty {
                return SharePayload.jpegData(data)
            }
            if pathExtension == "png",
               let data = try? Data(contentsOf: fileURL),
               !data.isEmpty {
                return SharePayload.pngData(data)
            }
        }

        if provider.hasItemConformingToTypeIdentifier(UTType.plainText.identifier),
           let text = try? await loadText(from: provider),
           !text.isEmpty {
            return SharePayload.text(text)
        }

        if provider.hasItemConformingToTypeIdentifier(UTType.url.identifier),
           let url = try? await loadURL(from: provider) {
            return SharePayload.text(url.absoluteString)
        }

        return nil
    }

    private func loadText(from provider: NSItemProvider) async throws -> String {
        let item = try await loadItem(from: provider, typeIdentifier: UTType.plainText.identifier)
        if let text = item as? String {
            return text
        }
        if let data = item as? Data,
           let text = String(data: data, encoding: .utf8) {
            return text
        }
        if let attributedText = item as? NSAttributedString {
            return attributedText.string
        }
        throw CocoaError(.fileReadCorruptFile)
    }

    private func loadURL(from provider: NSItemProvider, type: UTType = .url) async throws -> URL {
        let item = try await loadItem(from: provider, typeIdentifier: type.identifier)
        if let url = item as? URL {
            return url
        }
        if let url = item as? NSURL {
            return url as URL
        }
        if let data = item as? Data,
           let text = String(data: data, encoding: .utf8),
           let url = URL(string: text) {
            return url
        }
        if let text = item as? String,
           let url = URL(string: text) {
            return url
        }
        throw CocoaError(.fileReadCorruptFile)
    }

    private func loadImageData(from provider: NSItemProvider, type: UTType) async throws -> Data {
        if let data = try? await loadFileData(from: provider, type: type), !data.isEmpty {
            return data
        }

        return try await loadData(from: provider, type: type)
    }

    private func loadFileData(from provider: NSItemProvider, type: UTType) async throws -> Data {
        try await withCheckedThrowingContinuation { continuation in
            provider.loadFileRepresentation(forTypeIdentifier: type.identifier) { fileURL, error in
                if let error {
                    continuation.resume(throwing: error)
                    return
                }

                guard let fileURL else {
                    continuation.resume(throwing: CocoaError(.fileReadCorruptFile))
                    return
                }

                do {
                    continuation.resume(returning: try Data(contentsOf: fileURL))
                } catch {
                    continuation.resume(throwing: error)
                }
            }
        }
    }

    private func loadData(from provider: NSItemProvider, type: UTType) async throws -> Data {
        try await withCheckedThrowingContinuation { continuation in
            provider.loadDataRepresentation(forTypeIdentifier: type.identifier) { data, error in
                if let error {
                    continuation.resume(throwing: error)
                } else if let data {
                    continuation.resume(returning: data)
                } else {
                    continuation.resume(throwing: CocoaError(.fileReadCorruptFile))
                }
            }
        }
    }

    private func loadItem(from provider: NSItemProvider, typeIdentifier: String) async throws -> NSSecureCoding {
        try await withCheckedThrowingContinuation { continuation in
            provider.loadItem(forTypeIdentifier: typeIdentifier, options: nil) { item, error in
                if let error {
                    continuation.resume(throwing: error)
                } else if let item {
                    continuation.resume(returning: item)
                } else {
                    continuation.resume(throwing: CocoaError(.fileReadCorruptFile))
                }
            }
        }
    }

    private func ignoredText(_ count: Int, fallback: String?) -> String? {
        guard count > 0 else {
            return fallback
        }

        let itemText = plural(count, singular: "item", plural: "items")
        return "Ignored \(count) unsupported \(itemText)."
    }

    private func plural(_ count: Int, singular: String, plural: String) -> String {
        count == 1 ? singular : plural
    }
}
