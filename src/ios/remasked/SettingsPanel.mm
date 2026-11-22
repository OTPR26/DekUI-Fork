// SPDX-License-Identifier: GPL-2.0-or-later
#import "SettingsPanel.h"
#include <algorithm>
#include <cmath>
#include <memory>

namespace Menu = Oot3dNativeGame::Menu;

static UIColor* Accent() { return [UIColor colorWithRed:185/255.0 green:125/255.0 blue:242/255.0 alpha:1]; }
static UIColor* Surface() { return [UIColor colorWithRed:16/255.0 green:13/255.0 blue:23/255.0 alpha:1]; }
static UIColor* Control() { return [UIColor colorWithRed:36/255.0 green:28/255.0 blue:48/255.0 alpha:1]; }
static UIColor* Selected() { return [UIColor colorWithRed:61/255.0 green:20/255.0 blue:105/255.0 alpha:1]; }
static UIColor* Focused() { return [UIColor colorWithRed:74/255.0 green:31/255.0 blue:117/255.0 alpha:1]; }
static UIColor* Muted() { return [UIColor colorWithRed:176/255.0 green:166/255.0 blue:189/255.0 alpha:1]; }
static NSString* Text(const std::string& text) { return [NSString stringWithUTF8String:text.c_str()]; }
static CGFloat MenuScale() {
    NSNumber* value = [NSUserDefaults.standardUserDefaults objectForKey:@"ReMasked.Azahar.MenuScale"];
    return value ? std::clamp(value.doubleValue / 100.0, .50, 1.25) : 1;
}
static UIFont* Font(CGFloat size, bool bold = false) {
    size *= MenuScale();
    return [UIFont fontWithName:bold ? @"FiraSans-Bold" : @"FiraSans-Regular" size:size]
        ?: [UIFont systemFontOfSize:size weight:bold ? UIFontWeightBold : UIFontWeightRegular];
}

@interface ReMaskedSettingsPanel () <UITableViewDataSource, UITableViewDelegate> {
    std::unique_ptr<Menu::Model> _model;
    std::function<std::optional<Menu::Input>()> _input;
    void (^_close)(void);
    double _lastInputTime;
    bool _controllerFocus;
    bool _inGame;
    bool _leftHeld, _confirmHeld, _backHeld, _editingNumber;
    bool _suppressConfirm, _suppressBack;
}
@property(nonatomic,strong) UITableView* categories;
@property(nonatomic,strong) UITableView* rows;
@property(nonatomic,strong) UILabel* titleLabel;
@property(nonatomic,strong) UIView* contentPanel;
@property(nonatomic,strong) UILabel* categoryTitle;
@property(nonatomic,strong) UIButton* back;
@property(nonatomic,strong) CADisplayLink* inputDisplayLink;
@end

@implementation ReMaskedSettingsPanel
- (instancetype)initWithDocument:(Menu::Document)document
                           input:(std::function<std::optional<Menu::Input>()>)input
                          inGame:(BOOL)inGame
                           close:(void (^)(void))close {
    if ((self = [super init])) {
        _model = std::make_unique<Menu::Model>(std::move(document));
        _input = std::move(input);
        _inGame = inGame;
        _close = [close copy];
    }
    return self;
}

- (void)viewDidAppear:(BOOL)animated {
    [super viewDidAppear:animated];
    _lastInputTime = 0;
    if (!self.inputDisplayLink && _input) {
        self.inputDisplayLink = [CADisplayLink displayLinkWithTarget:self selector:@selector(updateController:)];
        [self.inputDisplayLink addToRunLoop:NSRunLoop.mainRunLoop forMode:NSRunLoopCommonModes];
    }
}

- (void)viewDidDisappear:(BOOL)animated {
    [super viewDidDisappear:animated];
    [self stopControllerInput];
}

- (void)stopControllerInput {
    [self.inputDisplayLink invalidate];
    self.inputDisplayLink = nil;
}

- (void)viewDidLoad {
    [super viewDidLoad];
    self.view.backgroundColor = [UIColor colorWithRed:8/255.0 green:7/255.0 blue:13/255.0 alpha:1];
    self.view.tintColor = Accent();
    self.overrideUserInterfaceStyle = UIUserInterfaceStyleDark;
    self.titleLabel = [UILabel new];
    self.titleLabel.adjustsFontSizeToFitWidth = YES;
    self.titleLabel.minimumScaleFactor = .65;
    [self.view addSubview:self.titleLabel];
    self.back = [UIButton buttonWithType:UIButtonTypeSystem];
    [self.back setTitleColor:UIColor.whiteColor forState:UIControlStateNormal];
    self.back.backgroundColor = Control();
    self.back.layer.cornerRadius = 4;
    [self.back addTarget:self action:@selector(goBack) forControlEvents:UIControlEventTouchUpInside];
    [self.view addSubview:self.back];

    self.categories = [[UITableView alloc] initWithFrame:CGRectZero style:UITableViewStylePlain];
    self.rows = [[UITableView alloc] initWithFrame:CGRectZero style:UITableViewStylePlain];
    for (UITableView* table in @[self.categories, self.rows]) {
        table.dataSource = self;
        table.delegate = self;
        table.backgroundColor = Surface();
        table.separatorColor = Surface();
        table.separatorInset = UIEdgeInsetsZero;
        table.estimatedRowHeight = 57;
        table.rowHeight = UITableViewAutomaticDimension;
        table.layer.cornerRadius = 5;
    }
    self.categories.separatorStyle = UITableViewCellSeparatorStyleNone;
    [self.view addSubview:self.categories];
    self.contentPanel = [UIView new];
    self.contentPanel.backgroundColor = Surface();
    self.contentPanel.layer.cornerRadius = 5;
    [self.view addSubview:self.contentPanel];
    self.categoryTitle = [UILabel new];
    self.categoryTitle.textColor = Accent();
    self.categoryTitle.adjustsFontSizeToFitWidth = YES;
    self.categoryTitle.minimumScaleFactor = .65;
    [self.contentPanel addSubview:self.categoryTitle];
    [self.contentPanel addSubview:self.rows];
    _model->Open();
    _model->FocusCategories();
    [self refreshAppearance];
}

- (void)viewDidLayoutSubviews {
    [super viewDidLayoutSubviews];
    const CGRect safe = UIEdgeInsetsInsetRect(self.view.bounds, self.view.safeAreaInsets);
    const CGFloat x = CGRectGetMinX(safe), y = CGRectGetMinY(safe), width = CGRectGetWidth(safe);
    const bool landscape = width > CGRectGetHeight(safe);
    const CGFloat sidebar = landscape ? std::clamp(width * .27, 110.0, 186.0)
                                     : std::clamp(width * .29, 90.0, 125.0);
    const CGFloat height = std::max(0.0, CGRectGetHeight(safe)-66);
    self.titleLabel.frame = CGRectMake(x + 16, y + 5, width - 105, 44);
    self.back.frame = CGRectMake(x + width - 90, y + 9, 82, 36);
    self.categories.frame = CGRectMake(x + 8, y + 54, sidebar, height);
    const CGFloat panelWidth = std::max(0.0, width - sidebar - 24);
    self.contentPanel.frame = CGRectMake(x + sidebar + 16, y + 54, panelWidth, height);
    self.categoryTitle.frame = CGRectMake(20, 3, std::max(0.0, panelWidth - 36), 35);
    self.rows.frame = CGRectMake(8, 41, std::max(0.0, panelWidth - 16), std::max(0.0, height - 46));
}

- (void)closePanel {
    [self stopControllerInput];
    [self dismissViewControllerAnimated:NO completion:_close];
}

- (void)goBack {
    if (_model->FocusPane() != Menu::Pane::Choices) {
        [self closePanel];
        return;
    }
    _model->SelectRow(_model->RowIndex());
    [self refreshAppearance];
    [self updateRowFocus:YES];
}

- (void)paintControllerFocus:(UITableViewCell*)cell at:(NSIndexPath*)index {
    const auto pane = _model->FocusPane();
    const auto focused = pane == Menu::Pane::Choices ? _model->ChoiceIndex() : _model->RowIndex();
    const bool highlighted = _controllerFocus && pane != Menu::Pane::Categories && index.row == NSInteger(focused);
    const auto& rows = _model->Doc().Tabs[_model->TabIndex()].Rows;
    const bool plain = pane != Menu::Pane::Choices &&
        (rows[index.row].Kind == Menu::RowKind::Heading || rows[index.row].Kind == Menu::RowKind::Info);
    cell.backgroundColor = highlighted ? (_editingNumber ? Focused() : Selected()) :
        (plain ? Surface() : Control());
}

- (void)updateRowFocus:(BOOL)scroll {
    const bool categoriesFocused = _controllerFocus && _model->FocusPane() == Menu::Pane::Categories;
    for (UITableViewCell* cell in self.categories.visibleCells)
        cell.selectedBackgroundView.backgroundColor = categoriesFocused ? Focused() : Selected();
    for (NSIndexPath* index in self.rows.indexPathsForVisibleRows)
        [self paintControllerFocus:[self.rows cellForRowAtIndexPath:index] at:index];
    if (!scroll || categoriesFocused) return;
    const auto index = _model->FocusPane() == Menu::Pane::Choices ? _model->ChoiceIndex() : _model->RowIndex();
    if (index < std::size_t([self.rows numberOfRowsInSection:0]))
        [self.rows scrollToRowAtIndexPath:[NSIndexPath indexPathForRow:index inSection:0]
                        atScrollPosition:UITableViewScrollPositionNone animated:NO];
}

- (void)updateController:(CADisplayLink*)sender {
    const double elapsed = _lastInputTime ? std::min(sender.timestamp - _lastInputTime, .1) : 0;
    _lastInputTime = sender.timestamp;
    if (self.presentedViewController || UIApplication.sharedApplication.applicationState != UIApplicationStateActive)
        return;
    const auto connected = _input();
    Menu::Input held = connected.value_or(Menu::Input{});
    const bool interacting = held.Up || held.Down || held.Left || held.Right ||
        held.Confirm || held.Back || held.TabPrevious || held.TabNext;
    const bool previouslyFocused = _controllerFocus;
    _controllerFocus = connected && (interacting || _controllerFocus);
    const auto tab = _model->TabIndex(), row = _model->RowIndex(), choice = _model->ChoiceIndex();
    const auto pane = _model->FocusPane();
    const int value = _model->FocusedRow() ? _model->FocusedRow()->Value : 0;
    const auto status = _model->Status();
    const bool wasEditingNumber = _editingNumber;
    const bool confirmPressed = held.Confirm && !_confirmHeld;
    const bool backPressed = held.Back && !_backHeld;
    const bool leftPressed = held.Left && !_leftHeld;
    _confirmHeld = held.Confirm;
    _backHeld = held.Back;
    _leftHeld = held.Left;
    _suppressConfirm &= held.Confirm;
    _suppressBack &= held.Back;
    if (_suppressConfirm) held.Confirm = false;
    if (_suppressBack) held.Back = false;
    if (!connected || held.TabPrevious || held.TabNext) _editingNumber = false;
    if (pane == Menu::Pane::Rows) {
        const Menu::Row* selected = _model->FocusedRow();
        if (_editingNumber) {
            if (confirmPressed || backPressed) {
                _editingNumber = false;
                _suppressConfirm = confirmPressed;
                _suppressBack = backPressed;
            }
            held.Up = held.Down = held.Confirm = held.Back = false;
            if (!_editingNumber) held.Left = held.Right = false;
        } else if (confirmPressed && selected && selected->Enabled && selected->Kind == Menu::RowKind::Number) {
            _editingNumber = true;
            held.Confirm = false;
        } else {
            if (leftPressed) _model->FocusCategories();
            held.Left = held.Right = false;
        }
    }
    _model->Update(held, elapsed);
    if (!_model->IsOpen()) {
        [self closePanel];
        return;
    }
    const bool tabChanged = tab != _model->TabIndex();
    const bool paneChanged = pane != _model->FocusPane();
    const bool focusChanged = row != _model->RowIndex() || choice != _model->ChoiceIndex();
    if (tabChanged || paneChanged || status != _model->Status() ||
        (_model->FocusedRow() && value != _model->FocusedRow()->Value))
        [self refreshAppearance];
    if (tabChanged) {
        [self.categories scrollToRowAtIndexPath:[NSIndexPath indexPathForRow:_model->TabIndex() inSection:0]
                              atScrollPosition:UITableViewScrollPositionNone animated:NO];
    }
    if (focusChanged || tabChanged || paneChanged || previouslyFocused != _controllerFocus ||
        wasEditingNumber != _editingNumber)
        [self updateRowFocus:YES];
    if (_model->StatusIsError() && !self.presentedViewController) {
        [self showError:Text(_model->Status())];
        _model->SetStatus({}, false);
    }
}

- (NSInteger)tableView:(UITableView*)table numberOfRowsInSection:(NSInteger)section {
    const auto& document = _model->Doc();
    if (table == self.rows && _model->FocusPane() == Menu::Pane::Choices)
        return _model->FocusedRow()->Choices.size();
    return table == self.categories ? document.Tabs.size() : document.Tabs[_model->TabIndex()].Rows.size();
}

- (UITableViewCell*)tableView:(UITableView*)table cellForRowAtIndexPath:(NSIndexPath*)index {
    UITableViewCell* cell = [[UITableViewCell alloc] initWithStyle:UITableViewCellStyleSubtitle reuseIdentifier:nil];
    cell.backgroundColor = Surface();
    cell.selectedBackgroundView = [UIView new];
    cell.selectedBackgroundView.backgroundColor = Selected();
    cell.textLabel.font = Font(16);
    cell.textLabel.textColor = UIColor.whiteColor;
    cell.textLabel.numberOfLines = 0;
    cell.detailTextLabel.font = Font(13);
    cell.detailTextLabel.textColor = Muted();
    cell.detailTextLabel.numberOfLines = 0;
    const auto& document = _model->Doc();
    if (table == self.categories) {
        cell.textLabel.text = Text(document.Tabs[index.row].Title);
        if (_controllerFocus && _model->FocusPane() == Menu::Pane::Categories)
            cell.selectedBackgroundView.backgroundColor = Focused();
        return cell;
    }
    [self paintControllerFocus:cell at:index];
    if (_model->FocusPane() == Menu::Pane::Choices) {
        const auto& row = *_model->FocusedRow();
        cell.textLabel.text = Text(row.Choices[index.row]);
        const bool enabled = !row.ChoiceAvailable || row.ChoiceAvailable(int(index.row));
        if (!enabled) cell.textLabel.textColor = UIColor.secondaryLabelColor;
        if (row.Value == index.row) cell.accessoryType = UITableViewCellAccessoryCheckmark;
        return cell;
    }

    const auto& row = document.Tabs[_model->TabIndex()].Rows[index.row];
    if (row.Kind == Menu::RowKind::Choice && !row.ChoiceImages.empty()) {
        UIStackView* choices = [UIStackView new];
        choices.axis = UILayoutConstraintAxisHorizontal;
        choices.distribution = UIStackViewDistributionFillEqually;
        choices.alignment = UIStackViewAlignmentCenter;
        choices.spacing = 6;
        choices.translatesAutoresizingMaskIntoConstraints = NO;
        [cell.contentView addSubview:choices];
        __weak ReMaskedSettingsPanel* panel = self;
        for (int i = 0; i < int(row.ChoiceImages.size()); ++i) {
            UIButton* icon = [UIButton buttonWithType:UIButtonTypeCustom
                primaryAction:[UIAction actionWithHandler:^(__kindof UIAction*) {
                    [panel writeRow:index.row value:i];
                }]];
            [icon setImage:[UIImage imageNamed:Text(row.ChoiceImages[i])] forState:UIControlStateNormal];
            icon.imageView.contentMode = UIViewContentModeScaleAspectFit;
            icon.clipsToBounds = YES;
            icon.layer.cornerRadius = 8;
            icon.layer.borderWidth = row.Value == i ? 3 : 0;
            icon.layer.borderColor = Accent().CGColor;
            icon.enabled = row.Enabled && (!row.ChoiceAvailable || row.ChoiceAvailable(i));
            icon.accessibilityLabel = Text(row.Choices[i]);
            if (row.Value == i) icon.accessibilityTraits |= UIAccessibilityTraitSelected;
            [icon.heightAnchor constraintEqualToAnchor:icon.widthAnchor].active = YES;
            [choices addArrangedSubview:icon];
        }
        [NSLayoutConstraint activateConstraints:@[
            [choices.leadingAnchor constraintEqualToAnchor:cell.contentView.layoutMarginsGuide.leadingAnchor],
            [choices.trailingAnchor constraintEqualToAnchor:cell.contentView.layoutMarginsGuide.trailingAnchor],
            [choices.topAnchor constraintEqualToAnchor:cell.contentView.topAnchor constant:12],
            [choices.bottomAnchor constraintEqualToAnchor:cell.contentView.bottomAnchor constant:-12]
        ]];
        cell.selectionStyle = UITableViewCellSelectionStyleNone;
        return cell;
    }
    cell.textLabel.text = Text(row.Key);
    cell.detailTextLabel.text = Text(row.Enabled ? row.Help : row.UnavailableReason);
    cell.selectionStyle = UITableViewCellSelectionStyleNone;
    if (!row.Enabled) cell.textLabel.textColor = UIColor.secondaryLabelColor;
    switch (row.Kind) {
    case Menu::RowKind::Heading:
        cell.textLabel.font = Font(16, true);
        cell.textLabel.textColor = Accent();
        break;
    case Menu::RowKind::Info:
        cell.detailTextLabel.text = Text(row.DynamicText ? row.DynamicText() : row.Text);
        break;
    case Menu::RowKind::Toggle: {
        UISwitch* toggle = [UISwitch new];
        toggle.on = row.Value != 0;
        toggle.onTintColor = Accent();
        toggle.enabled = row.Enabled;
        toggle.tag = index.row;
        toggle.accessibilityLabel = Text(row.Key);
        [toggle addTarget:self action:@selector(toggle:) forControlEvents:UIControlEventValueChanged];
        cell.accessoryView = toggle;
        break;
    }
    case Menu::RowKind::Number: {
        UIStackView* controls = [UIStackView new];
        controls.axis = UILayoutConstraintAxisVertical;
        controls.translatesAutoresizingMaskIntoConstraints = NO;
        UIStackView* labelRow = [UIStackView new];
        labelRow.alignment = UIStackViewAlignmentCenter;
        labelRow.spacing = 8;
        UILabel* label = [UILabel new];
        label.text = Text(row.Key);
        label.textColor = row.Enabled ? UIColor.whiteColor : Muted();
        label.font = Font(16);
        label.numberOfLines = 0;
        [labelRow addArrangedSubview:label];
        UILabel* value = [UILabel new];
        value.text = Text(row.ValueText());
        value.textColor = Accent();
        value.font = Font(13);
        [value setContentCompressionResistancePriority:UILayoutPriorityRequired
                                              forAxis:UILayoutConstraintAxisHorizontal];
        [labelRow addArrangedSubview:value];
        if (row.DefaultValue) {
            const int defaultValue = *row.DefaultValue;
            __weak ReMaskedSettingsPanel* panel = self;
            UIButton* reset = [UIButton buttonWithType:UIButtonTypeSystem
                primaryAction:[UIAction actionWithHandler:^(__kindof UIAction*) {
                    [panel writeRow:index.row value:defaultValue];
                }]];
            [reset setImage:[UIImage systemImageNamed:@"arrow.counterclockwise"]
                   forState:UIControlStateNormal];
            reset.accessibilityLabel = [@"Reset " stringByAppendingString:Text(row.Key)];
            reset.enabled = row.Enabled;
            [reset.widthAnchor constraintEqualToConstant:44].active = YES;
            [reset.heightAnchor constraintEqualToConstant:44].active = YES;
            [labelRow addArrangedSubview:reset];
        }
        UISlider* slider = [UISlider new];
        slider.minimumValue = row.Minimum;
        slider.maximumValue = row.Maximum;
        slider.value = row.Value;
        slider.enabled = row.Enabled;
        slider.tag = index.row;
        slider.accessibilityLabel = Text(row.Key);
        [slider addTarget:self action:@selector(slide:) forControlEvents:
            UIControlEventTouchUpInside | UIControlEventTouchUpOutside];
        [controls addArrangedSubview:labelRow];
        [controls addArrangedSubview:slider];
        const auto& detail = row.Enabled ? row.Help : row.UnavailableReason;
        if (!detail.empty()) {
            UILabel* help = [UILabel new];
            help.text = Text(detail);
            help.textColor = Muted();
            help.font = Font(13);
            help.numberOfLines = 0;
            [controls addArrangedSubview:help];
        }
        cell.textLabel.text = nil;
        cell.detailTextLabel.text = nil;
        [cell.contentView addSubview:controls];
        [NSLayoutConstraint activateConstraints:@[
            [controls.leadingAnchor constraintEqualToAnchor:cell.contentView.layoutMarginsGuide.leadingAnchor],
            [controls.trailingAnchor constraintEqualToAnchor:cell.contentView.layoutMarginsGuide.trailingAnchor],
            [controls.topAnchor constraintEqualToAnchor:cell.contentView.topAnchor constant:7],
            [controls.bottomAnchor constraintEqualToAnchor:cell.contentView.bottomAnchor constant:-7]
        ]];
        break;
    }
    case Menu::RowKind::Choice:
        cell.detailTextLabel.text = Text(row.ValueText() + (row.Help.empty() ? "" : "\n" + row.Help));
        [[fallthrough]];
    case Menu::RowKind::Action:
        if (row.Enabled) {
            cell.selectionStyle = UITableViewCellSelectionStyleDefault;
            cell.accessoryType = UITableViewCellAccessoryDisclosureIndicator;
        }
        break;
    }
    return cell;
}

- (void)writeRow:(NSInteger)index value:(int)value {
    _model->SelectRow(index);
    auto& row = _model->Doc().Tabs[_model->TabIndex()].Rows[index];
    if (!row.Enabled || !row.Write) return;
    const auto result = row.Write(value);
    _model->Refresh();
    [self refreshAppearance];
    if (result.Error) [self showError:Text(result.Message)];
}

- (void)refreshSettings {
    if (!self.isViewLoaded) return;
    _model->Refresh();
    [self refreshAppearance];
}

- (void)refreshAppearance {
    const auto* row = _model->FocusedRow();
    const bool choosing = _model->FocusPane() == Menu::Pane::Choices && row;
    NSString* brand = @"Majora ReMasked";
    NSMutableAttributedString* breadcrumb = [[NSMutableAttributedString alloc]
        initWithString:@"Majora ReMasked / Settings"
        attributes:@{NSForegroundColorAttributeName: UIColor.whiteColor, NSFontAttributeName: Font(19, true)}];
    [breadcrumb addAttribute:NSForegroundColorAttributeName value:Accent()
                       range:NSMakeRange(0, brand.length)];
    self.titleLabel.attributedText = breadcrumb;
    self.categoryTitle.text = choosing ? Text(row->Key) : Text(_model->Doc().Tabs[_model->TabIndex()].Title);
    self.categoryTitle.font = Font(16, true);
    [self.back setTitle:_inGame && !choosing ? @"Resume" : @"Back" forState:UIControlStateNormal];
    self.back.titleLabel.font = Font(15, true);
    [self.categories reloadData];
    [self.categories selectRowAtIndexPath:[NSIndexPath indexPathForRow:_model->TabIndex() inSection:0]
                                animated:NO scrollPosition:UITableViewScrollPositionNone];
    [self.rows reloadData];
}

- (void)showError:(NSString*)message {
    UIAlertController* alert = [UIAlertController alertControllerWithTitle:@"Settings" message:message
        preferredStyle:UIAlertControllerStyleAlert];
    [alert addAction:[UIAlertAction actionWithTitle:@"OK" style:UIAlertActionStyleDefault handler:nil]];
    [self presentViewController:alert animated:YES completion:nil];
}

- (void)toggle:(UISwitch*)toggle { [self writeRow:toggle.tag value:toggle.on]; }
- (void)slide:(UISlider*)slider {
    const auto& row = _model->Doc().Tabs[_model->TabIndex()].Rows[slider.tag];
    const int step = std::max(1, row.Step);
    const int value = row.Minimum + step * std::lround((slider.value - row.Minimum) / step);
    [self writeRow:slider.tag value:std::clamp(value, row.Minimum, row.Maximum)];
}

- (void)tableView:(UITableView*)table didSelectRowAtIndexPath:(NSIndexPath*)index {
    _editingNumber = false;
    if (table == self.categories) {
        _model->SelectTab(index.row);
        [self refreshAppearance];
        [self.rows setContentOffset:CGPointZero animated:NO];
        return;
    }
    [table deselectRowAtIndexPath:index animated:YES];
    if (_model->FocusPane() == Menu::Pane::Choices) {
        _model->SelectChoice(index.row);
        [self refreshAppearance];
        [self updateRowFocus:YES];
        if (_model->StatusIsError()) {
            [self showError:Text(_model->Status())];
            _model->SetStatus({}, false);
        }
        return;
    }
    _model->SelectRow(index.row);
    const auto& row = _model->Doc().Tabs[_model->TabIndex()].Rows[index.row];
    if (!row.Enabled) return;
    if (row.Activate) {
        const auto result = row.Activate();
        _model->Refresh();
        [self refreshAppearance];
        if (result.Error) [self showError:Text(result.Message)];
        return;
    }
    if (row.Kind != Menu::RowKind::Choice || !row.ChoiceImages.empty()) return;
    _model->OpenChoices();
    [self refreshAppearance];
    [self updateRowFocus:YES];
}
@end
