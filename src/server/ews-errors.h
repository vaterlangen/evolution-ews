#ifndef EWS_ERRORS_H
#define EWS_ERRORS_H

#include <glib.h>

G_BEGIN_DECLS

enum {
	ERROR_NONE,
	ERROR_ACCESSDENIED,
	ERROR_ACCOUNTDISABLED,
	ERROR_ADDDELEGATESFAILED,
	ERROR_ADDRESSSPACENOTFOUND,
	ERROR_ADOPERATION,
	ERROR_ADSESSIONFILTER,
	ERROR_ADUNAVAILABLE,
	ERROR_AFFECTEDTASKOCCURRENCESREQUIRED,
	ERROR_ATTACHMENTSIZELIMITEXCEEDED,
	ERROR_AUTODISCOVERFAILED,
	ERROR_AVAILABILITYCONFIGNOTFOUND,
	ERROR_BATCHPROCESSINGSTOPPED,
	ERROR_CALENDARCANNOTMOVEORCOPYOCCURRENCE,
	ERROR_CALENDARCANNOTUPDATEDELETEDITEM,
	ERROR_CALENDARCANNOTUSEIDFOROCCURRENCEID,
	ERROR_CALENDARCANNOTUSEIDFORRECURRINGMASTERID,
	ERROR_CALENDARDURATIONISTOOLONG,
	ERROR_CALENDARENDDATEISEARLIERTHANSTARTDATE,
	ERROR_CALENDARFOLDERISINVALIDFORCALENDARVIEW,
	ERROR_CALENDARINVALIDDAYFORTIMECHANGEPATTERN,
	ERROR_CALENDARINVALIDDAYFORWEEKLYRECURRENCE,
	ERROR_CALENDARINVALIDPROPERTYSTATE,
	ERROR_CALENDARINVALIDRECURRENCE,
	ERROR_CALENDARINVALIDTIMEZONE,
	ERROR_CALENDARISDELEGATEDFORACCEPT,
	ERROR_CALENDARISDELEGATEDFORDECLINE,
	ERROR_CALENDARISDELEGATEDFORREMOVE,
	ERROR_CALENDARISDELEGATEDFORTENTATIVE,
	ERROR_CALENDARISNOTORGANIZER,
	ERROR_CALENDARISORGANIZERFORACCEPT,
	ERROR_CALENDARISORGANIZERFORDECLINE,
	ERROR_CALENDARISORGANIZERFORREMOVE,
	ERROR_CALENDARISORGANIZERFORTENTATIVE,
	ERROR_CALENDARMEETINGREQUESTISOUTOFDATE,
	ERROR_CALENDAROCCURRENCEINDEXISOUTOFRECURRENCERANGE,
	ERROR_CALENDAROCCURRENCEISDELETEDFROMRECURRENCE,
	ERROR_CALENDAROUTOFRANGE,
	ERROR_CALENDARVIEWRANGETOOBIG,
	ERROR_CANNOTCREATECALENDARITEMINNONCALENDARFOLDER,
	ERROR_CANNOTCREATECONTACTINNONCONTACTFOLDER,
	ERROR_CANNOTCREATEPOSTITEMINNONMAILFOLDER,
	ERROR_CANNOTCREATETASKINNONTASKFOLDER,
	ERROR_CANNOTDELETEOBJECT,
	ERROR_CANNOTDELETETASKOCCURRENCE,
	ERROR_CANNOTOPENFILEATTACHMENT,
	ERROR_CANNOTSETCALENDARPERMISSIONONNONCALENDARFOLDER,
	ERROR_CANNOTSETNONCALENDARPERMISSIONONCALENDARFOLDER,
	ERROR_CANNOTSETPERMISSIONUNKNOWNENTRIES,
	ERROR_CANNOTUSEFOLDERIDFORITEMID,
	ERROR_CANNOTUSEITEMIDFORFOLDERID,
	ERROR_CHANGEKEYREQUIRED,
	ERROR_CHANGEKEYREQUIREDFORWRITEOPERATIONS,
	ERROR_CONNECTIONFAILED,
	ERROR_CONTENTCONVERSIONFAILED,
	ERROR_CORRUPTDATA,
	ERROR_CREATEITEMACCESSDENIED,
	ERROR_CREATEMANAGEDFOLDERPARTIALCOMPLETION,
	ERROR_CREATESUBFOLDERACCESSDENIED,
	ERROR_CROSSMAILBOXMOVECOPY,
	ERROR_DATASIZELIMITEXCEEDED,
	ERROR_DATASOURCEOPERATION,
	ERROR_DELEGATEALREADYEXISTS,
	ERROR_DELEGATECANNOTADDOWNER,
	ERROR_DELEGATEMISSINGCONFIGURATION,
	ERROR_DELEGATENOUSER,
	ERROR_DELEGATEVALIDATIONFAILED,
	ERROR_DELETEDISTINGUISHEDFOLDER,
	ERROR_DISTINGUISHEDUSERNOTSUPPORTED,
	ERROR_DUPLICATEINPUTFOLDERNAMES,
	ERROR_DUPLICATEUSERIDSSPECIFIED,
	ERROR_EMAILADDRESSMISMATCH,
	ERROR_EVENTNOTFOUND,
	ERROR_EXPIREDSUBSCRIPTION,
	ERROR_FOLDERCORRUPT,
	ERROR_FOLDEREXISTS,
	ERROR_FOLDERNOTFOUND,
	ERROR_FOLDERPROPERTREQUESTFAILED,
	ERROR_FOLDERSAVE,
	ERROR_FOLDERSAVEFAILED,
	ERROR_FOLDERSAVEPROPERTYERROR,
	ERROR_FREEBUSYGENERATIONFAILED,
	ERROR_IMPERSONATEUSERDENIED,
	ERROR_IMPERSONATIONDENIED,
	ERROR_IMPERSONATIONFAILED,
	ERROR_INCORRECTSCHEMAVERSION,
	ERROR_INCORRECTUPDATEPROPERTYCOUNT,
	ERROR_INDIVIDUALMAILBOXLIMITREACHED,
	ERROR_INSUFFICIENTRESOURCES,
	ERROR_INTERNALSERVERERROR,
	ERROR_INTERNALSERVERTRANSIENTERROR,
	ERROR_INVALIDACCESSLEVEL,
	ERROR_INVALIDATTACHMENTID,
	ERROR_INVALIDATTACHMENTSUBFILTER,
	ERROR_INVALIDATTACHMENTSUBFILTERTEXTFILTER,
	ERROR_INVALIDAUTHORIZATIONCONTEXT,
	ERROR_INVALIDCHANGEKEY,
	ERROR_INVALIDCLIENTSECURITYCONTEXT,
	ERROR_INVALIDCOMPLETEDATE,
	ERROR_INVALIDCROSSFORESTCREDENTIALS,
	ERROR_INVALIDDELEGATEPERMISSION,
	ERROR_INVALIDDELEGATEUSERID,
	ERROR_INVALIDEXCHANGEIMPERSONATIONHEADERDATA,
	ERROR_INVALIDEXCLUDESRESTRICTION,
	ERROR_INVALIDEXTENDEDPROPERTY,
	ERROR_INVALIDEXTENDEDPROPERTYVALUE,
	ERROR_INVALIDFOLDERID,
	ERROR_INVALIDFOLDERTYPEFOROPERATION,
	ERROR_INVALIDFRACTIONALPAGINGPARAMETERS,
	ERROR_INVALIDFREEBUSYVIEWTYPE,
	ERROR_INVALIDID,
	ERROR_INVALIDIDEMPTY,
	ERROR_INVALIDIDMALFORMED,
	ERROR_INVALIDIDMALFORMEDEWSLEGACYIDFORMAT,
	ERROR_INVALIDIDMONIKERTOOLONG,
	ERROR_INVALIDIDNOTANITEMATTACHMENTID,
	ERROR_INVALIDIDRETURNEDBYRESOLVENAMES,
	ERROR_INVALIDIDSTOREOBJECTIDTOOLONG,
	ERROR_INVALIDIDTOOMANYATTACHMENTLEVELS,
	ERROR_INVALIDINDEXEDPAGINGPARAMETERS,
	ERROR_INVALIDITEMFOROPERATIONACCEPTITEM,
	ERROR_INVALIDITEMFOROPERATIONCANCELITEM,
	ERROR_INVALIDITEMFOROPERATIONCREATEITEMATTACHMENT,
	ERROR_INVALIDITEMFOROPERATIONCREATEITEM,
	ERROR_INVALIDITEMFOROPERATIONDECLINEITEM,
	ERROR_INVALIDITEMFOROPERATIONEXPANDDL,
	ERROR_INVALIDITEMFOROPERATIONREMOVEITEM,
	ERROR_INVALIDITEMFOROPERATIONSENDITEM,
	ERROR_INVALIDITEMFOROPERATIONTENTATIVE,
	ERROR_INVALIDMANAGEDFOLDERPROPERTY,
	ERROR_INVALIDMANAGEDFOLDERQUOTA,
	ERROR_INVALIDMANAGEDFOLDERSIZE,
	ERROR_INVALIDMERGEDFREEBUSYINTERVAL,
	ERROR_INVALIDNAMEFORNAMERESOLUTION,
	ERROR_INVALIDNETWORKSERVICECONTEXT,
	ERROR_INVALIDOPERATION,
	ERROR_INVALIDPAGINGMAXROWS,
	ERROR_INVALIDPARENTFOLDER,
	ERROR_INVALIDPERCENTCOMPLETEVALUE,
	ERROR_INVALIDPERMISSIONSETTINGS,
	ERROR_INVALIDPROPERTYAPPEND,
	ERROR_INVALIDPROPERTYDELETE,
	ERROR_INVALIDPROPERTYFOREXISTS,
	ERROR_INVALIDPROPERTYFOROPERATION,
	ERROR_INVALIDPROPERTYREQUEST,
	ERROR_INVALIDPROPERTYSET,
	ERROR_INVALIDPROPERTYUPDATESENTMESSAGE,
	ERROR_INVALIDPULLSUBSCRIPTIONID,
	ERROR_INVALIDPUSHSUBSCRIPTIONURL,
	ERROR_INVALIDRECIPIENTS,
	ERROR_INVALIDRECIPIENTSUBFILTER,
	ERROR_INVALIDRECIPIENTSUBFILTERCOMPARISON,
	ERROR_INVALIDRECIPIENTSUBFILTERORDER,
	ERROR_INVALIDRECIPIENTSUBFILTERTEXTFILTER,
	ERROR_INVALIDREFERENCEITEM,
	ERROR_INVALIDREQUEST,
	ERROR_INVALIDROUTINGTYPE,
	ERROR_INVALIDSCHEDULEDOOFDURATION,
	ERROR_INVALIDSECURITYDESCRIPTOR,
	ERROR_INVALIDSENDITEMSAVESETTINGS,
	ERROR_INVALIDSERIALIZEDACCESSTOKEN,
	ERROR_INVALIDSID,
	ERROR_INVALIDSERVERVERSION,
	ERROR_INVALIDSMTPADDRESS,
	ERROR_INVALIDSUBSCRIPTION,
	ERROR_INVALIDSUBSCRIPTIONREQUEST,
	ERROR_INVALIDSYNCSTATEDATA,
	ERROR_INVALIDTIMEINTERVAL,
	ERROR_INVALIDUSERINFO,
	ERROR_INVALIDUSEROOFSETTINGS,
	ERROR_INVALIDUSERPRINCIPALNAME,
	ERROR_INVALIDUSERSID,
	ERROR_INVALIDVALUEFORPROPERTY,
	ERROR_INVALIDWATERMARK,
	ERROR_IRRESOLVABLECONFLICT,
	ERROR_ITEMCORRUPT,
	ERROR_ITEMNOTFOUND,
	ERROR_ITEMPROPERTYREQUESTFAILED,
	ERROR_ITEMSAVE,
	ERROR_ITEMSAVEPROPERTYERROR,
	ERROR_LOGONASNETWORKSERVICEFAILED,
	ERROR_MAILBOXCONFIGURATION,
	ERROR_MAILBOXDATAARRAYEMPTY,
	ERROR_MAILBOXDATAARRAYTOOBIG,
	ERROR_MAILBOXLOGONFAILED,
	ERROR_MAILBOXMOVEINPROGRESS,
	ERROR_MAILBOXSTOREUNAVAILABLE,
	ERROR_MAILRECIPIENTNOTFOUND,
	ERROR_MANAGEDFOLDERALREADYEXISTS,
	ERROR_MANAGEDFOLDERNOTFOUND,
	ERROR_MANAGEDFOLDERSROOTFAILURE,
	ERROR_MEETINGSUGGESTIONGENERATIONFAILED,
	ERROR_MESSAGEDISPOSITIONREQUIRED,
	ERROR_MESSAGESIZEEXCEEDED,
	ERROR_MIMECONTENTCONVERSIONFAILED,
	ERROR_MIMECONTENTINVALID,
	ERROR_MIMECONTENTINVALIDBASE64STRING,
	ERROR_MISSINGARGUMENT,
	ERROR_MISSINGEMAILADDRESS,
	ERROR_MISSINGEMAILADDRESSFORMANAGEDFOLDER,
	ERROR_MISSINGINFORMATIONEMAILADDRESS,
	ERROR_MISSINGINFORMATIONREFERENCEITEMID,
	ERROR_MISSINGITEMFORCREATEITEMATTACHMENT,
	ERROR_MISSINGMANAGEDFOLDERID,
	ERROR_MISSINGRECIPIENTS,
	ERROR_MISSINGUSERIDINFORMATION,
	ERROR_MORETHANONEACCESSMODESPECIFIED,
	ERROR_MOVECOPYFAILED,
	ERROR_MOVEDISTINGUISHEDFOLDER,
	ERROR_NAMERESOLUTIONMULTIPLERESULTS,
	ERROR_NAMERESOLUTIONNOMAILBOX,
	ERROR_NAMERESOLUTIONNORESULTS,
	ERROR_NOCALENDAR,
	ERROR_NODESTINATIONCASDUETOKERBEROSREQUIREMENTS,
	ERROR_NODESTINATIONCASDUETOSSLREQUIREMENTS,
	ERROR_NODESTINATIONCASDUETOVERSIONMISMATCH,
	ERROR_NOFOLDERCLASSOVERRIDE,
	ERROR_NOFREEBUSYACCESS,
	ERROR_NONEXISTENTMAILBOX,
	ERROR_NONPRIMARYSMTPADDRESS,
	ERROR_NOPROPERTYTAGFORCUSTOMPROPERTIES,
	ERROR_NORESPONDINGCASINDESTINATIONSITE,
	ERROR_NOTDELEGATE,
	ERROR_NOTENOUGHMEMORY,
	ERROR_OBJECTTYPECHANGED,
	ERROR_OCCURRENCECROSSINGBOUNDARY,
	ERROR_OCCURRENCETIMESPANTOOBIG,
	ERROR_OPERATIONNOTALLOWEDWITHPUBLICFOLDERROOT,
	ERROR_PARENTFOLDERNOTFOUND,
	ERROR_PASSWORDCHANGEREQUIRED,
	ERROR_PASSWORDEXPIRED,
	ERROR_PROPERTYUPDATE,
	ERROR_PROXIEDSUBSCRIPTIONCALLFAILURE,
	ERROR_PROXYGROUPSIDLIMITEXCEEDED,
	ERROR_PROXYREQUESTNOTALLOWED,
	ERROR_PROXYREQUESTPROCESSINGFAILED,
	ERROR_PUBLICFOLDERREQUESTPROCESSINGFAILED,
	ERROR_PUBLICFOLDERSERVERNOTFOUND,
	ERROR_QUERYFILTERTOOLONG,
	ERROR_QUOTAEXCEEDED,
	ERROR_READEVENTSFAILED,
	ERROR_READRECEIPTNOTPENDING,
	ERROR_RECURRENCEENDDATETOOBIG,
	ERROR_RECURRENCEHASNOOCCURRENCE,
	ERROR_REMOVEDELEGATESFAILED,
	ERROR_REQUESTSTREAMTOOBIG,
	ERROR_REQUIREDPROPERTYMISSING,
	ERROR_RESOLVENAMESINVALIDFOLDERTYPE,
	ERROR_RESOLVENAMESONLYONECONTACTSFOLDERALLOWED,
	ERROR_RESPONSESCHEMAVALIDATION,
	ERROR_RESTRICTIONTOOLONG,
	ERROR_RESTRICTIONTOOCOMPLEX,
	ERROR_RESULTSETTOOBIG,
	ERROR_SAVEDITEMFOLDERNOTFOUND,
	ERROR_SCHEMAVALIDATION,
	ERROR_SEARCHFOLDERNOTINITIALIZED,
	ERROR_SENDASDENIED,
	ERROR_SENDMEETINGCANCELLATIONSREQUIRED,
	ERROR_SENDMEETINGINVITATIONSORCANCELLATIONSREQUIRED,
	ERROR_SENDMEETINGINVITATIONSREQUIRED,
	ERROR_SENTMEETINGREQUESTUPDATE,
	ERROR_SENTTASKREQUESTUPDATE,
	ERROR_SERVERBUSY,
	ERROR_SERVICEDISCOVERYFAILED,
	ERROR_STALEOBJECT,
	ERROR_SUBSCRIPTIONACCESSDENIED,
	ERROR_SUBSCRIPTIONDELEGATEACCESSNOTSUPPORTED,
	ERROR_SUBSCRIPTIONNOTFOUND,
	ERROR_SYNCFOLDERNOTFOUND,
	ERROR_TIMEINTERVALTOOBIG,
	ERROR_TIMEOUTEXPIRED,
	ERROR_TIMEZONE,
	ERROR_TOFOLDERNOTFOUND,
	ERROR_TOKENSERIALIZATIONDENIED,
	ERROR_UNSUPPORTEDCULTURE,
	ERROR_UNSUPPORTEDMAPIPROPERTYTYPE,
	ERROR_UNSUPPORTEDMIMECONVERSION,
	ERROR_UNSUPPORTEDPATHFORQUERY,
	ERROR_UNSUPPORTEDPATHFORSORTGROUP,
	ERROR_UNSUPPORTEDQUERYFILTER,
	ERROR_UNSUPPORTEDRECURRENCE,
	ERROR_UNSUPPORTEDTYPEFORCONVERSION,
	ERROR_UPDATEDELEGATESFAILED,
	ERROR_UPDATEPROPERTYMISMATCH,
	ERROR_VIRUSDETECTED,
	ERROR_VIRUSMESSAGEDELETED,
	ERROR_WIN32INTEROPERROR,
	ERROR_NORESPONSE,
	ERROR_CANCELLED,
	ERROR_UNKNOWN
};

struct EwsErrorMap {
	const gchar *error_id;
	gint error_code;
};

gint ews_get_error_code (const gchar *str);

G_END_DECLS

#endif
