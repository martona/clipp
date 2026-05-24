//
//  ShareViewController.swift
//  ClippShareExtension
//

import UIKit

final class ShareViewController: UIViewController {
    private let doneButton = UIButton(type: .system)

    override func viewDidLoad() {
        super.viewDidLoad()

        view.backgroundColor = .systemBackground
        preferredContentSize = CGSize(width: 320, height: 260)

        let iconView = UIImageView(image: UIImage(systemName: "checkmark.circle.fill"))
        iconView.tintColor = .systemGreen
        iconView.contentMode = .scaleAspectFit
        iconView.translatesAutoresizingMaskIntoConstraints = false
        NSLayoutConstraint.activate([
            iconView.widthAnchor.constraint(equalToConstant: 44),
            iconView.heightAnchor.constraint(equalToConstant: 44),
        ])

        let titleLabel = UILabel()
        titleLabel.text = "Clipp received this share"
        titleLabel.font = .preferredFont(forTextStyle: .headline)
        titleLabel.adjustsFontForContentSizeCategory = true
        titleLabel.textAlignment = .center
        titleLabel.numberOfLines = 0

        let detailLabel = UILabel()
        detailLabel.text = "Sending from the share sheet will be wired up next."
        detailLabel.font = .preferredFont(forTextStyle: .subheadline)
        detailLabel.adjustsFontForContentSizeCategory = true
        detailLabel.textColor = .secondaryLabel
        detailLabel.textAlignment = .center
        detailLabel.numberOfLines = 0

        var configuration = UIButton.Configuration.filled()
        configuration.title = "Done"
        configuration.cornerStyle = .capsule
        configuration.contentInsets = NSDirectionalEdgeInsets(top: 10, leading: 20, bottom: 10, trailing: 20)
        doneButton.configuration = configuration
        doneButton.addTarget(self, action: #selector(doneTapped), for: .touchUpInside)

        let stackView = UIStackView(arrangedSubviews: [
            iconView,
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
        UIImpactFeedbackGenerator(style: .soft).impactOccurred()
    }

    @objc private func doneTapped() {
        extensionContext?.completeRequest(returningItems: [], completionHandler: nil)
    }
}
