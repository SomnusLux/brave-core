/* Copyright (c) 2019 The Brave Authors. All rights reserved.
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this file,
 * You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "brave/browser/ui/views/infobars/brave_wayback_machine_infobar_view.h"

#include <utility>
#include <vector>

#include "base/json/json_reader.h"
#include "brave/browser/brave_wayback_machine/brave_wayback_machine_infobar_delegate.h"
#include "brave/grit/brave_generated_resources.h"
#include "chrome/browser/infobars/infobar_service.h"
#include "chrome/browser/net/system_network_context_manager.h"
#include "chrome/browser/themes/theme_properties.h"
#include "chrome/browser/ui/views/chrome_layout_provider.h"
#include "chrome/browser/ui/views/chrome_typography.h"
#include "components/grit/components_scaled_resources.h"
#include "content/public/browser/navigation_controller.h"
#include "content/public/browser/web_contents.h"
#include "net/traffic_annotation/network_traffic_annotation.h"
#include "services/network/public/cpp/resource_request.h"
#include "services/network/public/cpp/shared_url_loader_factory.h"
#include "ui/base/l10n/l10n_util.h"
#include "ui/base/resource/resource_bundle.h"
#include "ui/base/theme_provider.h"
#include "ui/gfx/color_palette.h"
#include "ui/views/background.h"
#include "ui/views/controls/button/md_text_button.h"
#include "ui/views/controls/image_view.h"
#include "ui/views/controls/label.h"
#include "ui/views/controls/separator.h"
#include "ui/views/layout/flex_layout.h"
#include "ui/views/view_class_properties.h"
#include "url/gurl.h"

namespace {

constexpr char kWaybackQueryURL[] =
    "https://archive.org/wayback/available?url=";
constexpr int kMaxBodySize = 1024 * 1024;

// IDs of the colors to use for infobar elements.
constexpr int kInfoBarLabelBackgroundColor = ThemeProperties::COLOR_INFOBAR;
constexpr int kInfoBarLabelTextColor = ThemeProperties::COLOR_BOOKMARK_TEXT;

const net::NetworkTrafficAnnotationTag& GetNetworkTrafficAnnotationTag() {
  static const net::NetworkTrafficAnnotationTag network_traffic_annotation_tag =
      net::DefineNetworkTrafficAnnotation("wayback_machine_infobar", R"(
        semantics {
          sender:
            "Brave Wayback Machine"
          description:
            "Download wayback url"
          trigger:
            "When user gets 404 page"
          data: "current tab's url"
          destination: WEBSITE
        }
        policy {
          cookies_allowed: NO
          policy_exception_justification:
            "Not implemented."
        })");
  return network_traffic_annotation_tag;
}

}  // namespace

// Includes all view controls except close button that managed by InfoBarView.
class BraveWaybackMachineInfoBarView::InfoBarViewSubViews
    : public views::View,
      public views::ButtonListener {
 public:
  explicit InfoBarViewSubViews(BraveWaybackMachineInfoBarView* parent)
      : parent_(parent) {
    SetLayoutManager(std::make_unique<views::FlexLayout>());
    InitializeChildren();
  }

  ~InfoBarViewSubViews() override {
  }

  // views::View overrides:
  void OnThemeChanged() override {
    const SkColor background_color = GetColor(kInfoBarLabelBackgroundColor);
    const SkColor text_color = GetColor(kInfoBarLabelTextColor);
    for (auto* label : labels_) {
      label->SetBackgroundColor(background_color);
      label->SetEnabledColor(text_color);
    }

    separator_->SetColor(text_color);
  }

  void ButtonPressed(views::Button* sender, const ui::Event& event) override {
    parent_->FetchWaybackURL();
  }

  void OnWaybackURLFetchFailed() {
    UpdateChildrenVisibility(false);
  }

 private:
  using Labels = std::vector<views::Label*>;
  using Views = std::vector<views::View*>;

  void InitializeChildren() {
    // TODO(simonhong): Use real image assets.
    views::ImageView* image_view = new views::ImageView();
    image_view->SetImageSize(gfx::Size(100, 20));
    image_view->SetProperty(views::kMarginsKey,
                            gfx::Insets(12, 20, 12, 20));
    image_view->SetBackground(
        views::CreateSolidBackground(SkColorSetRGB(0xff, 0x76, 0x54)));
    AddChildView(image_view);

    separator_ = new views::Separator;
    separator_->SetProperty(views::kMarginsKey,
                            gfx::Insets(12, 0, 12, 20));
    AddChildView(separator_);

    const views::FlexSpecification label_flex_rule =
        views::FlexSpecification::ForSizeRule(
            views::MinimumFlexSizeRule::kScaleToMinimum,
            views::MaximumFlexSizeRule::kPreferred);
    auto* label = CreateLabel(
        l10n_util::GetStringUTF16(
            IDS_BRAVE_WAYBACK_MACHINE_INFOBAR_PAGE_MISSING_TEXT));
    label->SetFontList(
        label->font_list().DeriveWithWeight(gfx::Font::Weight::BOLD));
    views_visible_before_checking_.push_back(label);
    label->SetProperty(views::kFlexBehaviorKey, label_flex_rule.WithOrder(1));
    label->SetProperty(
        views::kMarginsKey,
        gfx::Insets(ChromeLayoutProvider::Get()->GetDistanceMetric(
                        DISTANCE_TOAST_LABEL_VERTICAL),
                    0));
    AddChildView(label);

    label = CreateLabel(l10n_util::GetStringUTF16(
        IDS_BRAVE_WAYBACK_MACHINE_INFOBAR_ASK_ABOUT_CHECK_TEXT));
    views_visible_before_checking_.push_back(label);
    label->SetProperty(
        views::kMarginsKey,
        gfx::Insets(ChromeLayoutProvider::Get()->GetDistanceMetric(
                        DISTANCE_TOAST_LABEL_VERTICAL),
                    5));
    label->SetElideBehavior(gfx::ELIDE_TAIL);
    label->SetProperty(views::kFlexBehaviorKey, label_flex_rule.WithOrder(2));
    AddChildView(label);

    // Add empty view to locate button to last.
    auto* place_holder_view = new views::View;
    views_visible_before_checking_.push_back(place_holder_view);
    place_holder_view->SetProperty(views::kMarginsKey, gfx::Insets(12, 0));
    place_holder_view->SetProperty(
        views::kFlexBehaviorKey,
        views::FlexSpecification::ForSizeRule(
            views::MinimumFlexSizeRule::kScaleToZero,
            views::MaximumFlexSizeRule::kUnbounded).WithOrder(3));
    AddChildView(place_holder_view);

    label = CreateLabel(
        l10n_util::GetStringUTF16(
            IDS_BRAVE_WAYBACK_MACHINE_INFOBAR_NOT_AVAILABLE_TEXT));
    views_visible_after_checking_.push_back(label);
    label->SetProperty(views::kFlexBehaviorKey, label_flex_rule);
    label->SetProperty(
        views::kMarginsKey,
        gfx::Insets(ChromeLayoutProvider::Get()->GetDistanceMetric(
                        DISTANCE_TOAST_LABEL_VERTICAL),
                    0));
    AddChildView(label);

    views::ImageView* sad_icon = new views::ImageView();
    views_visible_after_checking_.push_back(sad_icon);
    ui::ResourceBundle& rb = ui::ResourceBundle::GetSharedInstance();
    sad_icon->SetImage(rb.GetImageSkiaNamed(IDR_CRASH_SAD_FAVICON));
    sad_icon->SetProperty(views::kMarginsKey, gfx::Insets(12, 10));
    AddChildView(sad_icon);

    auto button = views::MdTextButton::CreateSecondaryUiBlueButton(
        this,
        l10n_util::GetStringUTF16(IDS_BRAVE_WAYBACK_MACHINE_CHECK_BUTTON_TEXT));
    auto* button_ptr = button.get();
    views_visible_before_checking_.push_back(button_ptr);
    button->SetProperty(
        views::kMarginsKey,
        gfx::Insets(ChromeLayoutProvider::Get()->GetDistanceMetric(
                        DISTANCE_TOAST_CONTROL_VERTICAL),
                    0));
    button->SizeToPreferredSize();
    AddChildView(button.release());

    UpdateChildrenVisibility(true);
  }

  views::Label* CreateLabel(const base::string16& text) {
    views::Label* label = new views::Label(text, CONTEXT_BODY_TEXT_LARGE);
    labels_.push_back(label);
    label->SetBackgroundColor(GetColor(kInfoBarLabelBackgroundColor));
    label->SetHorizontalAlignment(gfx::ALIGN_LEFT);
    label->SetEnabledColor(GetColor(kInfoBarLabelTextColor));
    return label;
  }

  void UpdateChildrenVisibility(bool show_before_checking_views) {
    for (auto* view : views_visible_before_checking_)
      view->SetVisible(show_before_checking_views);
    for (auto* view : views_visible_after_checking_)
      view->SetVisible(!show_before_checking_views);
  }

  SkColor GetColor(int id) const {
    const auto* theme_provider = GetThemeProvider();
    return theme_provider ? theme_provider->GetColor(id)
                          : gfx::kPlaceholderColor;
  }

  Labels labels_;
  Views views_visible_before_checking_;
  Views views_visible_after_checking_;

  views::Separator* separator_;
  BraveWaybackMachineInfoBarView* parent_;

  DISALLOW_COPY_AND_ASSIGN(InfoBarViewSubViews);
};

// static
std::unique_ptr<infobars::InfoBar>
BraveWaybackMachineInfoBarDelegate::CreateInfoBar(
    std::unique_ptr<BraveWaybackMachineInfoBarDelegate> delegate,
    content::WebContents* contents) {
  return std::make_unique<BraveWaybackMachineInfoBarView>(std::move(delegate),
                                                          contents);
}

BraveWaybackMachineInfoBarView::BraveWaybackMachineInfoBarView(
      std::unique_ptr<BraveWaybackMachineInfoBarDelegate> delegate,
      content::WebContents* contents)
    : InfoBarView(std::move(delegate)),
      contents_(contents),
      url_loader_factory_(SystemNetworkContextManager::GetInstance()->
          GetSharedURLLoaderFactory()) {
  sub_views_ = new InfoBarViewSubViews(this);
  sub_views_->SizeToPreferredSize();
  AddChildView(sub_views_);
}

BraveWaybackMachineInfoBarView::~BraveWaybackMachineInfoBarView() {
}

void BraveWaybackMachineInfoBarView::Layout() {
  InfoBarView::Layout();
  // |sub_views_| occupies from the beginning.
  sub_views_->SetBounds(0, OffsetY(sub_views_), EndX(), height());
}

void BraveWaybackMachineInfoBarView::FetchWaybackURL() {
  auto request = std::make_unique<network::ResourceRequest>();
  std::string wayback_fetch_url =
      std::string(kWaybackQueryURL) + contents_->GetVisibleURL().spec();
  request->url = GURL(wayback_fetch_url);
  wayback_url_fetcher_ = network::SimpleURLLoader::Create(
      std::move(request), GetNetworkTrafficAnnotationTag());
  wayback_url_fetcher_->DownloadToString(
      url_loader_factory_.get(),
      base::BindOnce(&BraveWaybackMachineInfoBarView::OnWaybackURLFetched,
                     base::Unretained(this)),
      kMaxBodySize);
}

void BraveWaybackMachineInfoBarView::OnWaybackURLFetched(
    std::unique_ptr<std::string> response_body) {
  if (!response_body) {
    sub_views_->OnWaybackURLFetchFailed();
    return;
  }

  std::string wayback_response_json = std::move(*response_body);
  const auto result = base::JSONReader::Read(wayback_response_json);
  if (!result || !result->FindPath("archived_snapshots.closest.url")) {
    sub_views_->OnWaybackURLFetchFailed();
    return;
  }

  LoadURL(result->FindPath("archived_snapshots.closest.url")->GetString());
  // After loading to archived url, don't need to show infobar anymore.
  InfoBarService::FromWebContents(contents_)->RemoveInfoBar(this);
}

void BraveWaybackMachineInfoBarView::LoadURL(
    const std::string& last_wayback_url) {
  DVLOG(2) << __func__ << ": wayback url(" << last_wayback_url
           << ") fetched for" << contents_->GetVisibleURL().spec();
  contents_->GetController().LoadURL(GURL(last_wayback_url),
                                     content::Referrer(),
                                     ui::PAGE_TRANSITION_LINK,
                                     std::string());
}
