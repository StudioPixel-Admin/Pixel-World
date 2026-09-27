# Private Launch Security and Exposure Guide

This project is intended as a private commercial game launch. The goal is to keep
launch-related information, source code, and operational data away from public
exposure until the project is intentionally released under an approved legal and
commercial plan.

## Core rule
Do not expose production, launch, or deployment data to public-facing environments
unless it is explicitly approved for release.

## Do not publish
The following should remain private until approved:
- repository URLs and internal Git remotes
- private build servers or CI configuration
- API keys, credentials, and tokens
- staging or production environment URLs
- internal testing accounts and test data
- private launch dates, sales targets, and revenue data
- patch notes, build metadata, or launcher configuration that includes internal
  references
- private Discord, Steam group, or platform access links not meant for general
  public release

## Steam and storefront rules
If a Steam page or storefront is used, keep the following out of public metadata:
- internal company-only launch dates
- unpublished milestone names or hidden development notes
- non-public build numbers that reveal internal testing states
- private beta keys or unreleased access information
- private links or credentials embedded in the build or launcher
- release pipeline details or staging configuration in packaged builds

Steam launch pages should be treated as public-facing materials. All public text,
images, and metadata should be reviewed for confidentiality before publication.

## Build and packaging controls
Before shipping any build:
- remove debug logs and developer-only paths
- strip internal server addresses, staging URLs, and machine names
- confirm no private credentials are embedded in configs, scripts, or assets
- avoid using repository or directory names that reveal internal projects
- check compiled binaries and installer metadata for internal references
- review crash reports, telemetry, and analytics for any private system data

## Privacy and data minimization
Only collect the data that is required for the game, support, or lawful business
operation. Avoid sending internal-only diagnostics or personal account metadata to
public services.

## Access control
Access to launch data should be restricted to the minimum number of authorized
people. Use a private project or private repository and avoid publishing docs that
contain release strategy, internal milestones, or operational information.

## Approval before public launch
Any public launch, storefront listing, website launch, or official distribution
must be approved in writing by the legal owner or authorized representative of
the project.

## Simple checklist
- No public repo with internal launch data
- No private keys in build artifacts
- No internal URLs in packaged builds
- No public Steam metadata beyond approved release content
- No public account, share code, or platform tokens
- No unapproved launch dates or private milestones

This document is a short operational control for private launch safety and should
be reviewed with legal and publishing counsel before any public release.
